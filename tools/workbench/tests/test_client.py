"""Unit tests of the Python client that need no running melonDS.

    python tools/workbench/tests/test_client.py
"""
import os
import queue
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dswb.client import Client  # noqa: E402


def client_without_connection():
    c = Client.__new__(Client)
    c.events = queue.Queue()
    c._handlers = []
    return c


class WaitEvent(unittest.TestCase):
    def test_returns_the_named_event(self):
        c = client_without_connection()
        c._dispatch("paused", {"frame": 1})
        c._dispatch("breakpoint_hit", {"id": 3})
        self.assertEqual(c.wait_event("breakpoint_hit", 1.0), {"id": 3})

    def test_times_out_with_timeout_error_when_no_event_arrives(self):
        c = client_without_connection()
        start = time.time()
        with self.assertRaises(TimeoutError):
            c.wait_event("breakpoint_hit", 0.2)
        self.assertLess(time.time() - start, 2.0)

    def test_times_out_with_timeout_error_when_only_other_events_arrive(self):
        c = client_without_connection()
        c._dispatch("paused", {})
        with self.assertRaises(TimeoutError):
            c.wait_event("breakpoint_hit", 0.2)


if __name__ == "__main__":
    unittest.main()
