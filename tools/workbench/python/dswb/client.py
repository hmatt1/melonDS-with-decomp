"""JSON-RPC client for the melonDS decomp workbench API.

    from dswb import Client
    with Client.connect() as c:          # reads <melonDS dir>/workbench/api.json
        print(c.status_get().state)
        c.execution_control("run_frames", count=60)
        for e in c.metadata_get(0x02000000, size=64).entries: ...

Every API method is available as a typed method generated from the schema
(memory_read, metadata_get, coverage_get, breakpoint_set, ...). Read methods go
to the read endpoint, control methods to the control endpoint.
"""
from __future__ import annotations

import json
import os
import queue
import socket
import threading
from typing import Any, Callable, Dict, List, Optional

from ._generated import GeneratedMethods, METHODS, EVENTS


class ApiError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def find_info_file(explicit: Optional[str] = None) -> str:
    """Locate api.json written by melonDS when the API starts."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("DSWB_API_INFO"):
        candidates.append(os.environ["DSWB_API_INFO"])
    if os.environ.get("MELONDS_DIR"):
        candidates.append(os.path.join(os.environ["MELONDS_DIR"], "workbench", "api.json"))
    candidates.append(os.path.join(os.getcwd(), "workbench", "api.json"))
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates.append(os.path.join(appdata, "melonDS", "workbench", "api.json"))
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    raise FileNotFoundError("api.json not found; start melonDS with --api or set DSWB_API_INFO. Tried: " + ", ".join(candidates))


class _Connection:
    def __init__(self, host: str, port: int, token: str, timeout: float, on_event: Callable[[str, Dict[str, Any]], None],
                 agent: str = ""):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(None)
        self.timeout = timeout
        self.lock = threading.Lock()
        self.next_id = 1
        self.pending: Dict[int, "queue.Queue[Dict[str, Any]]"] = {}
        self.on_event = on_event
        self.closed = False
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        self.auth = self.call("auth", {"token": token, "agent": agent})

    def _read_loop(self):
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(1 << 16)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    msg = json.loads(line)
                    if "id" in msg and msg["id"] is not None:
                        q = self.pending.pop(msg["id"], None)
                        if q is not None:
                            q.put(msg)
                    elif "method" in msg and str(msg["method"]).startswith("event."):
                        try:
                            self.on_event(msg["method"][6:], msg.get("params", {}))
                        except Exception:  # pragma: no cover - user callback
                            pass
        except OSError:
            pass
        finally:
            self.closed = True
            for q in list(self.pending.values()):
                q.put({"error": {"code": -1, "message": "connection closed"}})

    def call(self, method: str, params: Dict[str, Any], timeout: Optional[float] = None) -> Dict[str, Any]:
        q: "queue.Queue[Dict[str, Any]]" = queue.Queue()
        with self.lock:
            mid = self.next_id
            self.next_id += 1
            self.pending[mid] = q
            data = json.dumps({"jsonrpc": "2.0", "id": mid, "method": method, "params": params}).encode() + b"\n"
            self.sock.sendall(data)
        try:
            msg = q.get(timeout=timeout or self.timeout)
        except queue.Empty:
            self.pending.pop(mid, None)
            raise TimeoutError(f"{method} timed out")
        if "error" in msg:
            raise ApiError(msg["error"].get("code", -1), msg["error"].get("message", ""))
        return msg.get("result", {})

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


class Client(GeneratedMethods):
    """Typed client. Use Client.connect() to read the per-launch token from api.json."""

    def __init__(self, read_port: int, control_port: int, token: str, host: str = "127.0.0.1",
                 control: bool = True, timeout: float = 60.0, agent: str = ""):
        self.events: "queue.Queue[tuple]" = queue.Queue()
        self._handlers: List[Callable[[str, Dict[str, Any]], None]] = []
        self.read = _Connection(host, read_port, token, timeout, self._dispatch, agent)
        self.control = _Connection(host, control_port, token, timeout, self._dispatch, agent) if control else None

    @classmethod
    def connect(cls, info_file: Optional[str] = None, control: bool = True, timeout: float = 60.0, agent: str = "") -> "Client":
        """agent names the author of this client's knowledge-store edits (kb.* methods)."""
        path = find_info_file(info_file)
        with open(path, "r", encoding="utf-8") as f:
            info = json.load(f)
        return cls(info["read_port"], info["control_port"], info["token"], control=control, timeout=timeout, agent=agent)

    def _dispatch(self, name: str, params: Dict[str, Any]):
        self.events.put((name, params))
        for h in list(self._handlers):
            h(name, params)

    def on_event(self, handler: Callable[[str, Dict[str, Any]], None]):
        self._handlers.append(handler)

    def subscribe(self, events: Optional[List[str]] = None):
        """Subscribe to pushed events (all events when None). Events arrive once, on the read connection."""
        self.read.call("events.subscribe", {"events": events or []})

    def wait_event(self, name: str, timeout: float = 30.0) -> Dict[str, Any]:
        """Block until an event with this name arrives; earlier events are dropped."""
        import time
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(f"no {name} event")
            try:
                n, p = self.events.get(timeout=left)
            except queue.Empty:
                raise TimeoutError(f"no {name} event") from None
            if n == name:
                return p

    def _call(self, method: str, params: Dict[str, Any]) -> Dict[str, Any]:
        endpoint = METHODS[method][0]
        if endpoint == "control":
            if not self.control:
                raise ApiError(-32601, f"{method} needs the control endpoint")
            return self.control.call(method, params)
        return self.read.call(method, params)

    def call(self, method: str, **params) -> Dict[str, Any]:
        """Untyped call; returns the raw result dict."""
        return self._call(method, params)

    def close(self):
        self.read.close()
        if self.control:
            self.control.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
