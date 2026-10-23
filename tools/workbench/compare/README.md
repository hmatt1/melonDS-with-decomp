# Function compare

Compare tells you whether your C reconstruction of a function compiles to the original bytes. It
runs from the Source and diff panel (**Compare**, Ctrl+Shift+B), from **Build → Compare setup**,
and for agents through `kb.build.compare`. All three run the same command and record the same
result.

The workbench never compiles anything itself. It writes two inputs and runs the configured
command, which by default is this folder's `compare.py`:

1. **Target object.** The original function as an ELF relocatable: its bytes from RAM (or from the
   ROM, while another overlay holds its place), its symbol, mapping symbols, and the relocations
   the workbench can infer. These are calls to known functions, and literal-pool words that point
   at known functions, labels or named data objects. The diff then compares symbols, not
   addresses.
2. **Context header.** The types, prototypes and globals that the function's C file references,
   taken from the knowledge store. Your C file compiles without hand-written declarations. It
   covers every function whose source file is the same file.

`compare.py` then compiles the C file, checks that the function's symbol is in the compiled
object, diffs it against the target with objdiff-cli, and prints the result.

## What you need

| Tool | Notes |
|---|---|
| Python 3 | Any 3.x. `compare.py` uses only the standard library. |
| The game's compiler | For DS games, usually CodeWarrior's `mwccarm.exe`. The workbench never ships, downloads or links to it; use your own copy. |
| objdiff-cli 3.8 or newer | Required. Get it from [github.com/encounter/objdiff](https://github.com/encounter/objdiff/releases). |

## Setup

The Source and diff panel shows a checklist until everything passes. Each row has one button
that fixes it:

| Check | Fix |
|---|---|
| Python | Locate python.exe |
| Compiler (runs, and prints its version) | Browse for the compiler set for this module |
| objdiff-cli | Browse for objdiff-cli.exe |
| Source tree (set and writable) | Choose the folder holding the C files |
| Compiler settings for the function's module | Add settings for that module to `compare.json` |
| Self-test | Compiles a tiny built-in function against itself, which proves the toolchain works before your C is involved |

Compare can run once the first five rows pass. Agents get the same rows from `build.setup.check`.

The settings are split in two, so a project can be shared:

* **`compare.json`**, in the game's project folder (next to `knowledge.db`), is safe to share.
  It gives the compiler *version label* and flags for each module or address range:

  ```json
  {
    "version": 1,
    "default": {"compiler": "mwcc-2.0-sp1p5", "flags": "-O4,p -proc arm946e", "context": true},
    "modules": [
      {"module": "arm9", "compiler": "mwcc-2.0-sp1p5", "flags": "-O4,p -proc arm946e -interworking"},
      {"ranges": [["0x02000800", "0x02010000"]], "compiler": "mwcc-1.2-sp2", "flags": "-O4,s"}
    ]
  }
  ```

  Range rules win over module rules, and module rules win over the default. `"context": false`
  leaves out the generated header, for C files that include the project's own headers.

* **This computer's paths**, kept in melonDS's settings (`[Workbench]` in `melonDS.toml`):
  `PythonPath`, `ObjdiffPath`, `Compilers` (one `label=path` per compiler version label) and
  `CompareTimeoutSec` (default 60). Edit them under **Build → Compare setup → Settings**.

## The script

The first setup copies `compare.py` into the project folder as `compare/compare.py`. Your edits
there survive workbench updates. **Build → Compare setup → Script** edits it, shows the changes
from the template this build ships, and resets it to that template.

Compare shows the exact command before its first run, and again whenever the command or the
script changes. An agent's `kb.build.compare` is refused until a person has approved the current
command.

**Run on this function** (in the Run tab) runs the command once and shows stdout, stderr, the exit
code, the parsed result and any contract errors side by side. It records nothing unless you tick
**Record this attempt**.

## The command

The default command template is:

```
"{python}" "{script}" --file "{file}" --symbol {name} --target "{target}" --context "{context}"
  --compiler "{compiler}" --compiler-label "{compiler_label}" --flags "{flags}" --objdiff "{objdiff}" --timeout {timeout}
```

Set your own under **Build → Compare setup → Run**. Any tool that prints the contract below
works. The placeholders are:

| Placeholder | Value |
|---|---|
| `{addr}`, `{offset}`, `{module}`, `{name}`, `{function_id}` | The function: address, offset in its module, module, name, knowledge-store id |
| `{file}`, `{source_root}` | Its C file, and the source tree |
| `{kb}`, `{project}` | The knowledge store, and the project folder |
| `{target}`, `{context}` | The target object and context header the workbench wrote (`{context}` is empty when `context` is false) |
| `{compiler}`, `{compiler_label}`, `{flags}` | From `compare.json` and this computer's compiler paths |
| `{python}`, `{script}`, `{objdiff}`, `{timeout}` | This computer's tools, the project's script, and the timeout in seconds |

## Output contract

The command prints one JSON object on stdout:

```json
{"score": 87.5, "matched": false, "compiler": "mwcc-2.0-sp1p5", "flags": "-O4,p",
 "diff": [{"orig": "ldr r0, [pc, #0x8]", "compiled": "ldr r0, [pc, #0xc]", "status": "arg"}],
 "recorded": false, "stage": "done", "engine": "objdiff-cli 3.8.3", "log": ""}
```

* `score` is 0 to 100. `matched` defaults to `score == 100`.
* `diff` rows have a `status` of `match`, `diff`, `arg` (operands differ), `reloc` (points at a
  different symbol), `insert` or `delete`.
* `recorded: true` means the tool recorded the result itself, so the workbench only reloads.
* A failure prints `{"matched": false, "stage": "compile" | "extract" | "diff", "error": "...", "log": "..."}`.
  The workbench turns known failures into one sentence with a next step, and shows the raw output
  underneath.

`compare.py` never hangs or crashes: a timeout or any error still prints valid JSON.

## Tests

The tests need no proprietary tools: devkitARM's `arm-none-eabi-gcc` stands in for the compiler.

```
python tools/workbench/compare/test_compare_script.py          # the script: failures, timeouts, garbage
python tools/workbench/tests/test_compare_gcc.py               # end to end, against the test ROM's own C
```

The test ROM builds `testrom/match_src/wb_match.c` into the ROM. The end-to-end test matches those
functions byte for byte, and checks that `wb_match_wrong.c` does not match.
