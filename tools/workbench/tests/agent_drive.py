"""Small agent-style driver: send inputs through the API and save both screens after each step.

    python tools/workbench/tests/agent_drive.py --out DIR "wait:300" "a:5" "wait:60" "touch:128,96:5" ...

Steps: wait:N (run N frames), <buttons>:N (hold buttons, '+' separated, for N frames),
touch:X,Y:N, shot (save screens). Each step's screens are written to DIR/stepNN_top.png / _bottom.png.
Connects to a running melonDS started with --api.
"""
import argparse
import base64
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from dswb import Client  # noqa: E402


def save(out, n, top, bottom):
    for name, data in (("top", top), ("bottom", bottom)):
        if data:
            with open(os.path.join(out, f"step{n:02d}_{name}.png"), "wb") as f:
                f.write(base64.b64decode(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--info")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    with Client.connect(a.info) as c:
        for n, step in enumerate(a.steps):
            kind, _, rest = step.partition(":")
            if kind == "wait":
                r = c.execution_control("run_frames", count=int(rest or 1))
                sc = c.screen_capture()
                save(a.out, n, sc.top_png, sc.bottom_png)
                print(f"{n:02d} wait {rest} -> frame {sc.frame}")
            elif kind == "touch":
                xy, _, frames = rest.partition(":")
                x, y = (int(v) for v in xy.split(","))
                r = c.input_send(touch={"x": x, "y": y}, frames=int(frames or 3))
                save(a.out, n, r.top_png, r.bottom_png)
                print(f"{n:02d} touch {x},{y} -> frame {r.frame}")
            elif kind == "shot":
                sc = c.screen_capture()
                save(a.out, n, sc.top_png, sc.bottom_png)
                print(f"{n:02d} shot frame {sc.frame}")
            else:
                buttons = kind.split("+")
                r = c.input_send(buttons=buttons, frames=int(rest or 3))
                save(a.out, n, r.top_png, r.bottom_png)
                print(f"{n:02d} {kind} -> frame {r.frame}{' (overridden by the person)' if r.overridden else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
