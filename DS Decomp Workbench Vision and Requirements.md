# DS Decomp Workbench: Vision and Requirements

Oct 12, 2026 · @Matt

## Vision

The workbench is a complete decompilation interface for Nintendo DS games, built into the melonDS emulator. Its goal is a matching decompilation: C source that compiles with the original CodeWarrior toolchain into byte-identical machine code. A person can do the whole job in it alone, and AI agents can take on as much of the work as the person wants, through the same data. The first target is TrackMania DS, but nothing in the design is specific to one game.

Three kinds of evidence drive the work:

- **Runtime evidence.** An instrumented melonDS records what happened to every byte of memory: which instructions executed it, read it, wrote it or moved it by DMA, and in which CPU mode. This settles code versus data, ARM versus Thumb, overlay placement and data ownership by observation.
- **Static evidence.** Disassembly and cross-references show what code exists and how it connects.
- **Proof.** Compile-and-compare checks a C reconstruction byte for byte against the original. A function is done when its bytes match.

A project knowledge store ties the three together. It holds every confirmed fact keyed by address, and it is the shared memory between agent sessions and the people working on the project. Agents reach the emulator, the store and the toolchain through a small set of domain-specific MCP servers.

Three changes separate this design from the prototypes. melonDS becomes the primary interface, with disassembly, memory, registers, annotations and source diffing built in, so the external Python viewer and its 64GB shared-memory bridge go away. Work happens in real time: boot the game, play, pause and inspect, with recording on or off. And the knowledge store becomes the source of truth, with Binary Ninja or Ghidra output treated as one importable input among several.

Everything described in this document is in scope. The phased plan sets build order only, and nothing is deferred to a later release. The only target platform is Windows.

### Success criteria

- Every function in `arm9.bin` and every overlay is in one of three states: matched, partially matched or not started. Library code linked into the game is decompiled and matched from the binary like any other code.
- Every byte that executed during recorded play sessions belongs to a known function.
- An agent can start a session cold and continue the work from the knowledge store alone.
- A clean checkout of the decompiled source rebuilds a ROM whose `arm9.bin` and overlays are byte-identical to the original.

## Design principles

Each principle comes from a problem the prototypes hit.

| Principle | What it means | Prototype problem it answers |
| --- | --- | --- |
| Usable without AI | Every capability an agent has is also a panel, shortcut or command for a person. Agents only add speed. | The prototype's analysis lived mostly behind MCP tools, with a thin viewer on the side. |
| Real time first | Boot, play, pause and inspect live. Recording turns on and off at any moment. | The prototype viewer polled a mapping file and had no control over the game. |
| Observe, don't guess | Runtime records decide code vs data, instruction mode and ownership. Static heuristics are a fallback. | Static disassembly mislabels literal pools and Thumb code, and misses overlays entirely. |
| Record inside the emulator, view inside the emulator | The data and the views that read it live in one process. External tools query through an API. | The Python viewer remapped a 64KB window per address, roughly 400 `mmap` calls per second. |
| Pay only for what is touched | Recording storage is sparse and allocated on first touch. | A flat 64GB pagefile mapping for 512 bytes × 128M addresses. |
| Simple controls | Recording is one switch with two toggles. No modes to choose between. | Earlier plans mixed flags, counters and history into one heavy per-byte record. |
| One definition per data layout | Every shared structure has one schema that generates the code on both sides. | `ShadowByte` was declared twice, kept in sync by a comment and a `struct.unpack` format string. |
| Address is the universal key | Every fact in every component is keyed by `(region, address)`, with overlays named explicitly. | Static data covered `arm9.bin` only, and overlay code had no home. |
| Every pipeline step is verifiable | Each stage writes its outputs and a summary that a test can check. | A compile report counted successes as failures, and one pipeline stage never ran. |
| Matching bytes are the definition of done | A function's status changes only through a compile-and-compare result. | Status columns could be set by hand with no evidence attached. |
| Small, focused agent tools | Tools are grouped by domain, named for what the agent wants to do, and return compact results. | 75 tools on one server, several overlapping. |
| Windows only | The fork and all tooling build, run and are tested on Windows. Linux and macOS are out of scope. | Cross-platform support would add cost with no current user. |

## System overview

The system has four layers: agents and people at the top, MCP servers in the middle, three backends below them, and batch pipelines that feed the knowledge store.

&#91;embedded content: system architecture · 4 layers, 6 MCP servers\]

Agents act only through MCP servers, while people use melonDS directly. melonDS reads names from the knowledge store, and its session files reach the store through the session import pipeline. The build server also records every attempt in the store.

## Component 1: Recording engine in melonDS

The recording engine observes the running game in real time and keeps a per-byte record of what happened to memory. It replaces the shared-memory sidecar with storage that lives inside melonDS and is read by the built-in interface and the query API.

### Recording controls

Recording is one switch with two options. There are no other modes.

| Control | What it adds | Speed cost |
| --- | --- | --- |
| Recording (on or off) | Per-byte counters and flags, last writer, call edges, shadow call stack, provenance events, overlay tracking | Target: full speed in typical scenes |
| Write history (toggle, needs recording on) | Every write to every address: frame, cycle, CPU, PC, address, size, old value, new value. Kept in a log with a size cap set in settings; the oldest entries drop first. | More memory, small speed cost |
| Data origin (toggle, needs recording on) | Taint tracking from the sources you choose: all ROM files by default, or only specific files, bytes, ranges or registers you mark. Values from those sources carry their label wherever they move. | Grows with the amount of labeled data: a few marked sources run near full speed, all ROM files may run below it |

### Recording indicator

A person or agent must always know whether recording is on.

- A red `REC` badge in the status bar and `[REC]` in the window title whenever recording is on. Separate `HIST` and `ORIGIN` badges show when those toggles are on.
- The status bar shows frames recorded so far, write history size against its cap, and current emulation speed.
- Pause is shown separately (`PAUSED`), so recording-on-while-paused is never confused with recording-off.
- The API status call and `dsemu_status_get` report the same state, so an agent can check it before drawing conclusions.
- Every switch of recording or a toggle is logged with its frame number.

### What it records

| Event | Source hook | Recorded as |
| --- | --- | --- |
| Instruction fetch and execute, both CPUs | ARM9 and ARM7 execute loops | Exec count, CPU, ARM/Thumb flag, instruction-start flag |
| CPU loads and stores | Bus read and write paths | Read and write counts, last writer PC, last write frame |
| DMA transfers, all channels on both CPUs | `DMA::Run9`, `DMA::Run7` | DMA read and write counts, transfer source, channel |
| Calls and returns | `BL`, `BLX`, `BX LR`, `POP {…, PC}` | Caller-to-callee edges with counts, and a live shadow call stack per CPU |
| TCM layout changes | CP15 writes to the DTCM base and ITCM size registers | Timeline of TCM mappings |
| Game card reads | Card command and data port handlers | ROM offset to RAM destination, which identifies the NitroFS file loaded |
| BIOS decompression calls | SWI handlers for LZ77, Huffman and RLE | Compressed source range to decompressed destination range |
| Overlay loads | Writes into a code range followed by execution, compared with the overlays' ROM images | Which overlay occupied a RAM range, and when |

The last four events build **provenance**: a chain from a ROM file, through decompression, into RAM, and on to VRAM by DMA. That chain is what connects assets to the code that loads them.

### Data origin tracking

Data origin tracking follows values from the ROM to wherever they end up, so a field in a game struct can be traced back to the file it came from.

- **Sources you choose.** Only switched-on sources create labels. The default is every ROM file: bytes written by a card read are labeled with the NitroFS file, `arm9.bin` or overlay they came from. A person or agent can narrow that to specific files, or add a source at any moment by selecting bytes or a range in the memory view, or a register at the current PC, and choosing Track this value. Each source has a name that appears on everything derived from it. Keypad, touch and RTC input are optional sources.
- **Propagation covers the whole machine.** A tracked value can move through any register or address, so propagation runs everywhere while the toggle is on. Limiting it to chosen addresses would lose the trail the moment a value moved somewhere else.
- **Cost follows the labels.** When no operand of an instruction carries a label, the propagation step is skipped. Tracking one struct or one file therefore costs far less than tracking every ROM file.
- **Propagation rules.** Every register on both CPUs carries a label. Loads copy the memory label into the register, stores copy it back, ALU operations combine their operands' labels, and DMA and BIOS decompression copy labels from source to destination.
- **Constants.** Values loaded from literal pools inside executed code are labeled as code constants, so they don't flood results with the module label.
- **Labels.** Each distinct set of sources is stored once in a label table. Bytes and registers hold a 32-bit label ID in separate sparse pages, allocated only while the toggle is on.
- **Scope.** It tracks data flow. It does not taint values because a branch depended on a tainted value, since that marks nearly everything as derived from everything.

### Storage model

- **Physical addressing.** Records are keyed by physical location (region plus offset), and views translate to DS virtual addresses. Main RAM mirrors and relocated TCMs collapse onto one record, so a byte is never counted twice under two addresses.
- **Compact byte record.** Each touched byte gets a fixed record of roughly 32 bytes: five counters, last writer PC, last write frame, flags and memory class. The prototype used 512 bytes per byte.
- **Sparse pages.** Byte records and taint labels are allocated in 4KB pages on first touch, through a two-level page table. Untouched address space costs nothing.
- **Write history log.** An append-only log, filtered at query time by address, frame range and CPU. This replaces the 14-entry ring buffer stored inside every byte.
- **Overlay epochs.** RAM reused by different overlays keeps a separate record per overlay, keyed by overlay ID and offset, so evidence from one overlay never blends into another.
- **One schema.** The record layouts are defined once in a schema file. The C++ structs, the API types and the Python bindings are generated from it.

### Sessions

The intended flow is real time. A person or an agent boots the game, plays it, and pauses whenever they want to look at something. Everything recorded so far is visible in the interface and queryable through the API while the game runs or sits paused.

A session runs from when recording turns on until it turns off or the game closes. At that point melonDS saves a session file (byte records, write history, call edges, provenance and labels) tagged with the ROM's SHA-1 and frame range, and imports the function-level evidence into the knowledge store. Sessions merge, so coverage from many play sessions combines into one picture. Savestates and melonDS's existing input recording work during a session, but neither is required.

### Requirements

- Recording is off by default. With it off, emulation speed matches unmodified melonDS.
- Recording state is always visible, as described under Recording indicator.
- With recording on and both toggles off, a typical 3D scene runs at full speed on a current desktop CPU.
- **Interpreter only.** Recording works only with the JIT recompiler disabled, because JIT-compiled code bypasses the hooks. Adding hooks to the JIT is not planned. The fork ships with the JIT off. When the JIT is on, the recording controls stay disabled and show a message saying to turn the JIT off.
- **Windows only.** The fork, the recording engine and all tooling target Windows. Linux and macOS are out of scope.

## Component 2: Decompilation interface in melonDS

melonDS becomes a full decompilation interface for DS games. A person can do the whole job in it with no agent involved: play, pause, inspect, name, annotate, define types and check C against the original. Agents are optional collaborators that work on the same data, and everything they add shows up in the same views.

### Who it serves

- **A person working alone.** Every capability an agent has through MCP is also available as a panel, a menu action or a keyboard shortcut.
- **A person working with agents.** Agent findings and edits appear live, marked with their author, and the person can take over at any point.
- **Agents.** They read and change the same data through the API and the MCP servers.

### Default layout

The disassembly sits in the center with the memory view under it. Registers, Inspector and Annotations sit on the right, Functions and Bookmarks on the left, and the remaining panels are tabs along the bottom. Every panel docks, floats or closes, and layouts are saved.

### Panels

| Panel | What it shows | Key requirements |
| --- | --- | --- |
| Disassembly | Rows around the current address: the PC when locked, otherwise the cursor | Each row shows address, raw bytes, mnemonic, operands, annotation and a heat bar. Decoder chosen from the recorded Thumb flag. Never-executed bytes shown as data words. PC row green, cursor row blue, PC corrected for pipeline offset. Branch targets and addresses clickable. |
| Memory | Hex and ASCII around the current address, 256 bytes by default | Heat coloring, zero bytes dimmed, annotated bytes underlined, known structs overlaid with field names. Mirrors and TCM mappings labeled. |
| Registers | All registers, CPSR flags and mode for ARM9 and ARM7 | Each register shows its annotation for the current function, such as `r4: player state`. Clicking a value that is an address jumps there. |
| Inspector | The value at the selected address | u8, u16, u32, s32, fixed-point and binary decodes. Counters, write history entries with links to the writer, data origin. |
| Annotations | Every annotation in view or in the project | Filter by author, target kind and tag. Add, edit and delete. |
| Functions | Known and discovered functions | Status, call count, size and mode. Executed code outside any function flagged. Create, resize and rename. |
| Call stack | The live shadow call stack for each CPU | Available while recording. Click a frame to jump. |
| Call graph | Observed callers and callees of the selected function | Edge counts, expand one level at a time. |
| Cross-references | References to the selected address | Static references from the import plus observed reads, writes and calls. |
| Coverage map | Whole regions, one pixel per N bytes | Color by exec, read, write or DMA. Filter to all time, this session or the last N frames. |
| Write history | Writes to the selected address or range | Frame, CPU, writer PC, old and new value. Click to jump to the writer. |
| Data origin | Where the selected bytes came from | ROM file, decompression step and DMA hop, plus taint labels when that toggle is on. |
| Overlays | Which overlays occupy which RAM, over time | Load events with frame numbers. Switch the disassembly between overlay epochs. |
| Source and diff | The C source for the current function | Read from the git tree. Last build score. Side-by-side instruction diff against the original. A Compare button runs the build check, and Open in editor opens the file in the person's editor. |
| Decompiler | Pseudo-C from the static analysis import | Read-only reading aid, with knowledge-store names applied. |
| Search | Bytes, strings (ASCII and Shift-JIS) and values | A value scan narrows candidates across pauses (changed, unchanged, equal to) to find variables. |
| Breakpoints | Execute, read and write breakpoints | Conditions, hit counts, hit list. |
| Recording | Recording switch, write history and data origin toggles | The indicator described in Component 1. |

### Annotations

Annotations let a person or an agent attach knowledge to anything they are looking at.

- **Targets.** An address or range, a function, a data object, a struct field, a type, a register within a function or PC range, an overlay or a savestate.
- **Content.** A short comment plus optional tags.
- **Authorship.** Each annotation records its author (a person or an agent session) and time. Agent annotations carry a distinct marker, and every list filters by author.
- **Where they appear.** Address annotations show in the disassembly and memory views. Register annotations show in the Registers panel and as operand hints in the disassembly when that register appears in the annotated function.
- **Storage.** Annotations live in the knowledge store, so they persist across sessions and agents see the same ones through `kb` tools.

### Interaction

- Keyboard shortcuts cover the common actions: go to address, rename, comment, define function, define data, back and forward, lock to PC, pause.
- Every edit goes through the same knowledge-store write path the agents use, with the author recorded, and can be undone.
- The prototype viewer's conveniences carry over: lock to PC, back and forward history, address entry and clickable addresses in operands.

### Boundary with external tools

The compiler and the diff tool run as separate processes. melonDS calls them and shows the results in the Source and diff panel. This keeps compiler and toolchain code out of the fork, so it still rebases onto upstream melonDS.

## Component 3: Control and query API in melonDS

melonDS exposes a local, versioned API so that MCP servers and scripts can drive the emulator and query recordings. It replaces both the shared-memory bridge and GDB as the agent's way in. The GDB stub stays for people who want a debugger.

### Capabilities

| Group | Operations | Kind |
| --- | --- | --- |
| Status | Emulator state, recording state and toggles, loaded ROM hash, frame number, PC and registers per CPU, current TCM layout | Read |
| Memory | Read a range by virtual address, with typed decode | Read |
| Metadata | Byte records for a range, aggregated per instruction or per N bytes | Read |
| Write history | Writes to an address or range, filtered by frame window and CPU | Read |
| Coverage | Executed ranges, optionally only those outside known functions | Read |
| Calls | Observed callers and callees of an address, and the current shadow call stack | Read |
| Data origin | Provenance chain and taint labels of a range | Read |
| Screen | Capture both screens as PNG | Read |
| Game | Boot the ROM, reset | Control |
| Execution | Pause, resume, run N frames then pause, step one instruction | Control |
| Input | Press buttons and touch the screen for N frames | Control |
| State | Load and save savestates | Control |
| Breakpoints | Execute, read and write breakpoints, with conditions | Control |
| Recording | Turn recording on and off, toggle write history and data origin, add and remove data origin sources | Control |
| Memory write | Write a range | Control, disabled by default |

### Requirements

- **Range queries only.** Every query takes a range and returns aggregated results in one round trip. Nothing requires one call per byte.
- **Read and control on separate endpoints.** The split lets the MCP layer offer a read-only server without exposing anything that changes emulator state.
- **Generated bindings.** Request and response types come from the same schema file as the recording structures, with C++ and Python generated from it.
- **Events.** Breakpoint hits, pauses and recording changes are pushed to subscribers, so callers don't poll.
- **Local only.** The API listens on localhost, requires a per-launch token, and is off unless enabled in settings or on the command line.
- **Shared control.** When an agent drives the game, the window stays open and live, so a person can watch, pause and take over. Input from the person always wins over agent input.

The transport is an open question (JSON-RPC over a local socket or gRPC). The schema-first requirement holds either way.

## Component 4: Project knowledge store

The knowledge store holds every fact the project has established, keyed by address, with the evidence behind it. It has two parts: a SQLite database for facts and status, and a git repository for the decompiled C source.

### Why two parts

The prototype stored C implementations in a database column. Source code belongs in git, where it can be diffed, reviewed, built as a whole and branched. The database records which function lives in which source file and what the last build said about it.

### Entities

| Entity | Holds | Key |
| --- | --- | --- |
| Module | `arm9`, `arm7`, each overlay: load address, size, ROM file, compression | Module ID |
| Function | Start, end, mode, name, signature, subsystem, status | (module, offset) |
| Data object | Name, type, size, owner function | (module or region, offset) |
| Type | Structs, enums and typedefs in C form | Name |
| Label | Named addresses that are neither functions nor data objects | (module, offset) |
| Annotation | Comment and tags attached to an address, range, function, data object, struct field, type, register within a function, overlay or savestate, with author | Entity reference |
| Task | Work item with priority and state, attached to any entity | Task ID |
| Runtime evidence | Per-function summary from imported sessions: exec count, callers, callees, data touched | (function, session) |
| Build result | Every compile-and-compare attempt: compiler, flags, score, diff, commit | Attempt ID |
| Asset | NitroFS files, formats, and provenance links to RAM ranges and loader functions | File ID |
| Change log | Who changed what, when, and from which agent session | Row ID |

### Requirements

- **Integer addresses.** Addresses are stored as integers within a module. Hex strings appear only at the edges, in tools and UIs.
- **Enforced vocabularies.** Status, mode, subsystem and similar fields are lookup tables or `CHECK` constraints. The prototype's subsystem list lived only in Python code.
- **Evidence-backed status.** A function's status is derived from its latest build result. Nothing sets `matched` by hand.
- **Versioned migrations.** Schema changes run through numbered migrations checked into the repository, replacing one-off scripts like `new_columns.py`.
- **Attribution.** Every write records the author (a person or an agent session ID), so the history of any name or type can be traced.
- **Concurrency.** WAL mode, so melonDS can read while MCP servers write.
- **Importers.** Adapters load function boundaries, cross-references and decompiler output from Ghidra or Binary Ninja. Decompiler text is kept as a cached reading aid, never as the source of truth.
- **Exporters.** Symbol files for Ghidra and Binary Ninja, and the symbol map and linker inputs the decomp build needs, are generated from the store.

## Component 5: Build and match validation

The build component turns a C reconstruction into a verdict. It compiles one function or a whole module with the original toolchain and compares the result against the game's bytes.

### Function-level compare

1. Generate a context: the types, globals and callee prototypes the function needs, pulled from the knowledge store and the source tree.
2. Compile the candidate C with the compiler and flags configured for that module.
3. Extract the function's object code and relocations.
4. Diff against the original with relocation awareness: a relocated operand matches when it points at the same symbol, whatever its numeric value.
5. Return an instruction-level diff and a score, and record the attempt as a build result.

An existing diff engine such as objdiff, which matching-decomp projects already use, may cover steps 3 and 4.

### Module-level verify

The source tree builds and links into `arm9.bin` and each overlay. A byte-for-byte comparison against the originals is the final gate for the project and runs on every merge.

### Requirements

- **Per-module compiler configuration.** Library code and game code may have been built with different CodeWarrior versions and flags. The project config maps each module, or each address range, to its compiler and flags.
- **Windows toolchain.** `mwccarm.exe` is a Windows program and runs natively, which matches the project's Windows-only scope.
- **Same check for people and agents.** A person runs the compare from the Source and diff panel in melonDS or from a command line. An agent runs it through `build_function_compare`. All three call the same service and record the same result.
- **Deterministic and cached.** The same source, compiler and flags always produce the same verdict, and identical requests reuse the last result.
- **History, not overwrite.** Every attempt is kept, so anyone can see what was tried before and which change raised the score.

## Component 6: MCP server layer

Agents reach the system through six domain servers with about 42 tools in total, replacing one server with 75. Each server owns one domain, and servers that change state are separate from servers that only read.

### Rules taken from the AWS guidance

| Rule | Source | How it applies here |
| --- | --- | --- |
| Bound servers by domain, split by separation of duties | [Tool organization](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-organization.html) | Emulator, knowledge, build and assets are separate domains. Emulator and knowledge each split into read and write servers. |
| Name tools `domain_noun_verb` | [Tool organization](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-organization.html) | `kb_function_search`, `dsemu_breakpoint_set`. Related tools sort together and never collide across servers. |
| Split a server past 50 tools | [Tool organization](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-organization.html) | Target 10 or fewer per server. No server here exceeds 9. |
| Load only the tools a task needs | [Tool discovery](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-discovery.html) | Agent profiles load a subset of servers per task type (below). The guide estimates 250 to 500 tokens per tool definition. |
| Bundle operations that commonly happen together | [Tool scope](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-scope.html) | When a workflow needs three or more calls, one tool does it. `kb_work_next` returns a whole work packet. |
| Eight parameters or fewer, with defaults | [Tool scope](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-scope.html) | Address is usually the only required input. Frame windows, decode types and limits have defaults. |
| Separate read and write tools | [Tool scope](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-scope.html) | Enforced at server level, so a read-only profile cannot change anything. |
| Prefer deterministic tools, agents as tools for open-ended work | [Tool scope](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-scope.html) | Every tool is deterministic. The match loop itself runs as a worker agent (see workflow). |
| Full specs: `name`, `description`, `inputSchema`, `outputSchema`, enums | [Tool definitions](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-definitions.html) | Pydantic models for every input and output. Enums for status, mode, decode type and direction. |
| Write descriptions as prompts, with examples and dependencies | [Tool definitions](https://docs.aws.amazon.com/prescriptive-guidance/latest/mcp-strategies/mcp-tool-strategy-definitions.html) | Each description states what the tool does, when to use it, what the output is for, one example with real values, and which tool to call before or after. |

### Servers and tools

| Server | Access | Tools |
| --- | --- | --- |
| `dsemu-observe` | Read | `dsemu_status_get`, `dsemu_memory_read`, `dsemu_address_profile`, `dsemu_writes_list`, `dsemu_calls_get`, `dsemu_coverage_unknown_list`, `dsemu_origin_get`, `dsemu_screen_capture` |
| `dsemu-control` | Write | `dsemu_game_boot`, `dsemu_execution_control`, `dsemu_input_send`, `dsemu_recording_configure`, `dsemu_breakpoint_set`, `dsemu_breakpoint_clear`, `dsemu_state_save`, `dsemu_state_load`, `dsemu_memory_write` (off by default) |
| `kb-read` | Read | `kb_work_next`, `kb_address_describe`, `kb_function_get`, `kb_function_search`, `kb_xrefs_list`, `kb_type_get`, `kb_annotation_search`, `kb_history_list`, `kb_progress_get` |
| `kb-write` | Write | `kb_function_annotate`, `kb_dataobject_record`, `kb_type_define`, `kb_label_set`, `kb_annotation_save`, `kb_task_create`, `kb_task_update`, `kb_review_flag` |
| `build` | Write | `build_context_get`, `build_function_compare`, `build_attempt_list`, `build_function_commit`, `build_module_verify` |
| `assets` | Read | `asset_file_list`, `asset_file_describe`, `asset_file_provenance` |

The `dsemu-control` tools support the same real-time flow a person uses: boot the game, send button and touch input, pause, inspect, resume. `dsemu_execution_control` takes one action (pause, resume, step, or run N frames then pause), and `dsemu_recording_configure` sets the recording switch, both toggles and the data origin sources in one call. `kb_annotation_save` takes any annotation target, including a register within a function.

Importing static analysis and importing sessions are batch jobs with their own command-line entry points. Agents don't need them as tools.

Game-specific helpers, such as the prototype's TrackMania resource table dump, go in an optional per-game server that a project enables in its config.

### Workflow tools that replace prototype tool chains

| New tool | Replaces | What it returns |
| --- | --- | --- |
| `kb_work_next` | `get_validation_queue`, `suggest_functions_to_analyze`, `get_recently_worked_functions`, then several lookups | The next function to work on, plus disassembly, decompiler draft, callers and callees with names, runtime evidence, annotations and prior attempts |
| `kb_address_describe` | `get_address_summary`, `get_address_context` and related lookups | Everything known about one address, in one compact record |
| `build_function_compare` | `compile_and_compare`, `save_compilation_result`, `update_function_validation`, `auto_validate_function` | Score, instruction diff and the recorded attempt ID |
| `dsemu_address_profile` | Several `gdb_execute` calls plus manual decoding | Code or data, mode, counters, recent writers, data origin and overlay for one address or range |
| `dsemu_input_send` | No prototype equivalent | Presses buttons or touches the screen for N frames, then returns the frame number and both screens |

### Agent profiles

Each profile is a client configuration that loads only the servers its task needs.

| Profile | Servers loaded | Used for |
| --- | --- | --- |
| Matcher | `kb-read`, `kb-write`, `build` | Writing and iterating on C for one function |
| Investigator | `dsemu-observe`, `dsemu-control`, `kb-read`, `kb-write`, `assets` | Answering runtime questions: who writes a field, what loads a model |
| Reviewer | `kb-read`, `dsemu-observe` | Read-only checks of names, types and flagged functions |
| Planner | `kb-read`, `kb-write` | Choosing work, creating tasks, reorganizing subsystems |

### Tool quality

A set of real prompts with the expected tool calls is kept as a regression suite. Changes to tool names or descriptions run against it, so tool selection accuracy doesn't drift.

## End-to-end workflow

A project moves from a ROM to matched source in five steps. Steps 2 to 4 repeat for the life of the project.

1. **Set up the project.** A config file names the ROM, the compilers per module and the database path. An import job extracts `arm9.bin`, `arm7.bin`, the overlays and NitroFS, records the modules, and loads a static analysis import.
2. **Play and record in real time.** A person or an agent boots the game in melonDS, turns recording on, and plays. They pause whenever something is worth a look, inspect it in the interface, annotate it, and resume. When recording turns off, the session's evidence goes into the knowledge store.
3. **Match functions.** A person picks a function in the Functions panel, writes C in their editor, and checks it with Compare in the Source and diff panel. Agents do the same through MCP: an orchestrating agent hands work packets from `kb_work_next` to matcher agents, which write C, call `build_function_compare` and revise until the function matches or a retry budget runs out.
4. **Answer runtime questions.** When the meaning of a field is unclear, a person or an investigator agent sets a write breakpoint, plays until it hits, and records which code writes the field and with what values.
5. **Review and verify.** People review agent work in melonDS, where agent annotations and names are marked by author. `build_module_verify` runs on every merge to confirm the whole module still rebuilds byte for byte.

Prioritization favors the most-executed and most-called functions first, because their names and types unlock their callers.

## Non-functional requirements

| Area | Requirement |
| --- | --- |
| Performance | Recording off: no measurable slowdown versus upstream melonDS. Recording on with both toggles off: full speed in typical scenes on a current desktop. Write history and data origin may slow emulation, and the status bar shows the current speed. API range queries over 64KB return in tens of milliseconds. Interface panels redraw only what changed. |
| Platform | Windows only. The fork, MCP servers, pipelines and the CodeWarrior toolchain all run natively on Windows. Linux and macOS are out of scope. |
| Emulation mode | Recording requires the interpreter. The JIT recompiler is off by default and recording is unavailable while it is on. |
| Usability | Every agent capability has a panel, shortcut or command-line equivalent. Recording state is visible at all times. |
| Traceability | Every session, match result and annotation records the ROM hash, tool versions and author that produced it. |
| Data integrity | Schema changes go through migrations. Writes carry an author and can be undone. Status derives from evidence. |
| Testability | Each pipeline has tests on a small fixture. MCP tools have a prompt regression suite. The recording engine has a test ROM with known code, data, Thumb regions and data flows for taint. |
| Security | The melonDS API binds to localhost with a per-launch token. Write servers are separate from read servers. Memory writes are off unless enabled. |
| Maintainability | The melonDS changes live in a small set of new files plus minimal hooks, so the fork rebases onto upstream releases. |
| Licensing | The melonDS fork stays under GPLv3. No proprietary Nintendo code, libraries or headers are used, committed or redistributed, and neither are the ROM or extracted assets. The public repository holds only original code, decompiled source and tooling. |

## Phased delivery plan

All seven phases are in scope. The order reflects dependencies, and each phase ends with a check that proves it works before the next one starts.

1. **Foundations.** Project config, knowledge store schema and migrations, ROM import, shared schema with code generation.
   - Exit: the ROM imports, every module is listed, and a static analysis import loads.
2. **Recording core.** Recording switch and indicator, sparse byte records, exec/read/write/DMA counters, mode and instruction-start flags, physical addressing, call edges and shadow call stack, session files.
   - Exit: a test ROM records exactly its known code, data and Thumb regions, and a TrackMania DS race records at full speed with the indicator showing.
3. **Decompilation interface.** Disassembly with bytes, memory, registers, inspector, annotations, functions, call stack, cross-references, coverage map, search and breakpoints.
   - Exit: a person finds, names and annotates a function, its data and its registers with no agent involved, and the Python viewer is retired.
4. **API and emulator servers.** The control and query API, `dsemu-observe` and `dsemu-control`, session import into the knowledge store.
   - Exit: an agent boots the game, plays to a scene with button input, pauses, and answers "which code writes this address".
5. **Build loop and knowledge servers.** Function compare, module verify, the Source and diff panel, `kb-read`, `kb-write`, `build`.
   - Exit: a person matches one function from the Source and diff panel, and an agent matches another through MCP.
6. **Provenance, overlays and write history.** Write history toggle, overlay epochs, card read and decompression tracking, the Write history, Overlays and Data origin panels, `assets` server.
   - Exit: for a chosen 3D model file, the interface shows its ROM file, its RAM location and the function that loaded it.
7. **Data origin tracking.** Taint labels, register and memory propagation, the data origin toggle.
   - Exit: a field in a game struct traces back to the ROM file it came from.

## Open questions

Answered on 2026-10-14 for components 1–3, from the ROM and the workbench as built. Questions that belong to components 4–6 stay open, with what is known so far.

- [ ] Which CodeWarrior version and flags built the game code, and do library modules differ?
  - **Still open (component 5).** The ROM has no compiler strings: no `Metrowerks`, `CodeWarrior` or `mwcc`. CodeWarrior for DS 2.0 is installed locally and is the first candidate; the version, service pack and flags have to come from matching compiled functions.
- [ ] Static analysis importer: keep Binary Ninja, which the prototypes used, or move to Ghidra headless, which is free to run in automation?
  - **The melonDS side is decided:** File > Import static analysis and `kb.import.static_analysis` read a tool-neutral JSON file (functions, labels, xrefs, decompiled text, with a `tool` field), so either tool can feed it.
  - **The exporter is still open (component 4).** Recommendation: Ghidra headless for the automated pipeline, since it runs unattended without a licence. A Binary Ninja exporter can produce the same JSON for people who use it.
- [x] Overlay detection: hook the game's overlay-loading functions once identified, or detect load-then-execute patterns generically?
  - **Generic, no hooks.** An overlay counts as loaded when the card reads its file into RAM, or when code that was written runs and matches the overlay's ROM image ("written, then executed"; compressed overlays are compared with their unpacked image). This also covers savestates and doesn't depend on identifying the loader functions first.
- [x] Default size cap for the write history log.
  - **4 million entries in memory** (about 128 MB), changeable in settings. Or a memory-mapped history file of any size, at about 32,000 writes per MB; 512 MB holds a TrackMania boot-to-race with the default filters. Filters drop bulk copies, stack writes and unchanged writes by default. Each saved session keeps its whole history as `<session>.dswbh`, within a disk budget of 2 GB per game, and is searched one session at a time.
- [x] API transport: JSON-RPC over a local socket, or gRPC.
  - **JSON-RPC 2.0 over TCP on 127.0.0.1**, one JSON object per line. There are two endpoints, read (port 47800 by default) and control (port + 1), and a per-launch token in `workbench/api.json`. Types are generated from the shared schema (`wbgen.py`) for C++ and Python.
- [ ] Reuse existing matching-decomp tools (objdiff for diffing, an existing DS decomp build system) or build our own?
  - **Still open (component 5).** melonDS doesn't commit to either: Compare runs a configurable external command that prints a small JSON result (score, matched, compiler, flags, diff rows). A wrapper around `objdiff-cli` fits that contract, so reusing objdiff needs no melonDS changes.
- [x] Should melonDS read the knowledge store's SQLite file directly, or go through a small local service?
  - **Directly.** melonDS opens `knowledge.db` (SQLite in WAL mode) itself, with schema migrations, attribution and per-author undo. Agents write through the API's `kb.*` methods, so edits from people and agents share one write path. Other tools can read the file directly or through the API.
