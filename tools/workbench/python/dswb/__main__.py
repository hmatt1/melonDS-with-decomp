"""Command line front end:  python -m dswb <method> [json params]

    python -m dswb status.get
    python -m dswb memory.read '{"addr": "0x02000000", "size": 32, "decode": "u32"}'
    python -m dswb execution.control '{"action": "run_frames", "count": 60}'
    python -m dswb --session path/to/file.dswbs      (summary of a session file)
"""
import json
import sys

from .client import Client
from . import session as sessionmod
from ._generated import METHODS


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        print("methods:", ", ".join(sorted(METHODS)))
        return 0
    if argv[1] == "--session":
        s = sessionmod.load(argv[2])
        h = s.header
        print(f"rom {s.rom_sha1}  frames {h.frame_start}-{h.frame_end}  sections {h.section_count}")
        pages = list(s.pages())
        print(f"byte record pages: {len(pages)}")
        print(f"call edges: {len(s.call_edges())}  provenance: {len(s.provenance())}  writes: {len(s.write_history())}")
        print(f"overlay events: {len(s.overlay_events())}  sources: {len(s.sources())}")
        return 0
    method = argv[1]
    params = json.loads(argv[2]) if len(argv) > 2 else {}
    with Client.connect() as c:
        print(json.dumps(c.call(method, **params), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
