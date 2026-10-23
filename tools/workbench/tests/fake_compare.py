"""Stand-in for the build service's compare command, used by the UI test.

Prints the JSON contract the Source and diff panel expects:
{"score", "matched", "compiler", "flags", "diff": [{"orig", "compiled", "status"}], "recorded": false}
A source file containing "/* MATCH */" scores 100, anything else 82.5.
"""
import argparse
import json

ap = argparse.ArgumentParser()
ap.add_argument("--function")
ap.add_argument("--file")
a = ap.parse_args()
src = open(a.file, encoding="utf-8").read() if a.file else ""
match = "/* MATCH */" in src
rows = [
    {"orig": "ldr r1, [pc, #0xc]", "compiled": "ldr r1, [pc, #0xc]", "status": "match"},
    {"orig": "ldr r2, [r1]", "compiled": "ldr r2, [r1]", "status": "match"},
    {"orig": "ldr r3, [r1, #4]", "compiled": "ldr r3, [r1, #4]" if match else "ldr r2, [r1, #4]", "status": "match" if match else "diff"},
    {"orig": "add r0, r2, r3", "compiled": "add r0, r2, r3" if match else "add r0, r3, r2", "status": "match" if match else "diff"},
    {"orig": "bx lr", "compiled": "bx lr", "status": "match"},
]
print(json.dumps({"score": 100.0 if match else 82.5, "matched": match, "compiler": "mwccarm 2.0/sp1p5 (stand-in)",
                  "flags": "-O4,p -proc arm946e", "diff": rows, "recorded": False}))
