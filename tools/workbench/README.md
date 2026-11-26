# DS decomp workbench

This fork of melonDS adds three things for decompiling DS games, following
*DS Decomp Workbench Vision and Requirements.md*:

1. **A recording engine.** It records what the code does to every byte of RAM, the calls
   between functions, where data came from, and the history of writes.
2. **A decompilation interface.** A second window with disassembly, memory, annotations and
   analysis panels, backed by a per-game knowledge store.
3. **A control and query API.** A local JSON-RPC API that lets scripts and agents drive the
   emulator and query recordings.

The person and the agent see the same data. Anything the API can do, the window can also do
from a panel, a menu or a shortcut.

**Building** needs, beyond melonDS's own dependencies, SQLite, zlib and the Qt Advanced Docking
System. On MSYS2 UCRT64, `pacman -S mingw-w64-ucrt-x86_64-qt-advanced-docking-system` installs
ADS. The window's panels are ADS dock widgets: they dock, tab, float, auto-hide to a side bar and
close, and the layout is saved as it changes.

**Savestates.** Loading a state replaces memory, so data-origin labels are dropped and rebuilt from
content:
* `arm9.bin`, `arm7.bin` and overlays where they still equal the ROM;
* copies of them, such as relocated code or ITCM autoload;
* archived and packed ROM files, wherever their bytes are.

Everything else has no origin. The Data origin panel and `origin.get` (`state_loaded_frame`) say
so. Coverage, counts and calls are kept.

**Coverage map.**
* Ctrl+wheel zooms toward the mouse, from a whole region down to single bytes; + and - zoom too,
  and Fit shows the whole region.
* The wheel scrolls; middle-drag pans.
* Click or drag selects. The Inspector, Data origin and Write history follow the selection, and
  double-click opens the Disassembly or Memory.
* Hovering a cell says what it is: names, counts, mode, how it decodes, and the first frame it was
  executed, read, written and DMA'd this session. The Inspector gives the same for one byte.
* **Layers** draws functions, data objects, loaded overlays, bookmarks and the PC and cursor over
  the map. *Archived files (slower)* outlines main RAM that equals a ROM file or an archived member
  (for example `arm9.bin` or a `vfs.zip` entry), and the tooltip names it. It matches only the
  visible range, and again when the view moves or the game has run for a second.
* **Combined** colours each cell by its busiest access.

**Game screen.** View > *Game screen in the workbench* (Ctrl+Shift+G) runs the game inside the Game screen panel,
with its own menus and screen options, and *Hide the separate game window* hides the separate
window. Both renderers work, and the panel can float.

**Help.** The Getting started panel, shown until you untick *Show at startup*, explains Recording,
Write history and Data origin. They are on from boot. Help > Feature requirements
lists what each feature needs and whether it is on now.

**ARM, Thumb or data.** Every address has one classification, shown the same way in the
Disassembly's mode gutter, the Inspector, the Functions list and the API.

| Confidence | When |
|---|---|
| confirmed | It ran in that mode; or it was only ever read, or it is a literal pool, and never ran |
| override | Someone set it with **Treat as** (right-click in the Disassembly) or `kb.decode.set` |
| defined | It is inside a knowledge-store data object |
| inferred | It is inside a knowledge-store function that never ran |
| none | Nothing is known: it shows as a muted `.word` |

* The gutter badge (A, T, M, D or ?) is solid when confirmed and outlined otherwise. Hovering it
  says why.
* An override the recording disagrees with is marked with a red **!**. **Annotations > decode
  conflicts** lists them.
* **Decode only confirmed code** in the Disassembly's context menu shows everything else as data.

**Tables and trees.** Double-click goes to the row's address. Right-click offers the same actions
everywhere (Functions, Annotations, Bookmarks, Call stack, Call graph, Cross-references, Write
history, Overlays, Search, Breakpoints, a register holding an address, and the coverage map): go to
the Disassembly or Memory, rename, comment, add bookmark, cross-references, show write history, show
data origin, track value origin and copy the address, under the panel's own actions. They are the
menu bar's commands, with the same names and keys.

**Layout.**
* **View menu:** lists the panels by purpose (Code, Runtime, Tracing, Project). Each item's tooltip,
  and each tab's, says what the panel shows.
* **Window > Panels... (Ctrl+Shift+P):** finds a panel by typing part of its name or description,
  then opens and focuses it.
* **Window > Workspaces:** switches between named layouts. The built-ins are Default, Decomp,
  Debug, Data tracing, Coverage and Minimal; Ctrl+Alt+1 to 9 switch to the first nine.
  * A workspace keeps your changes to it, along with the window's size and position and the game
    screen mode it was left in. **Reset to default** undoes layout changes.
  * **Save current as...** makes a new workspace. You can also rename, delete, export or import one
    as JSON.
  * A `workspaces.json` in the project folder adds a project's own workspaces.
* **Restoring:** a layout saved before a panel existed gets that panel in its default place.
  Floating panels left on a missing monitor come back to the window's screen.

## Layout

| Path | What it is |
|---|---|
| `src/workbench/` | Core: recorder, data origin (taint), debugger, NitroROM parser, instruction decoder, session file format |
| `src/workbench/schema/workbench.schema.json` | **Single source of truth** for record layouts, enums, API types, methods and events |
| `tools/workbench/wbgen.py` | Generator: `WBRecords.h` (core), `WBApi.h/.cpp` (frontend), `python/dswb/_generated.py`. The CMake build runs it |
| `src/frontend/qt_sdl/workbench/` | Host glue (`WBHost`), API server, knowledge store, the workbench window, panels, dialogs, unit tests |
| `src/frontend/qt_sdl/workbench/kb/*.sql` | Knowledge store migrations, applied in order |
| `tools/workbench/python/dswb/` | Python client (typed, generated from the schema), session reader, CLI, and the dsd tools: `dsd.py` (reads a dsd configuration), `dsd_import.py`, `dsd_sync.py`, `dsd_check.py`, `vtables.py` |
| `tools/workbench/testrom/` | Test ROM with known code, data, Thumb regions and data flows, on the ARM9 and its own ARM7 core (`arm7/`) |
| `tools/workbench/tests/` | Integration tests and benchmarks |

The emulator core changes are small hooks:

* `CPUExecuteMode::InterpreterWB` adds pre-exec and post-exec hooks to the interpreter.
* The ARM data read and write paths call into the recorder.
* There are hooks for DMA, card commands, TCM and WRAM mapping, reset, and direct boot.

With the workbench off, none of these hooks run.

## Running

```
melonDS [options] game.nds
  -w, --workbench       open the workbench window on startup
  -wo, --workbench-only open the workbench with the game in its Game screen panel and the main
                        window hidden, for this run; closing the workbench quits melonDS
  --default-layout      open the workbench with the default layout, not the one used last
  --api                 start the API on 127.0.0.1
  --api-port <port>     read endpoint port (control = port+1, default 47800/47801)
  --api-memory-write    allow memory.write through the API
  --record              turn recording on once the game boots (write history and data origin stay off)
  --write-history       turn recording and write history on
  --data-origin         turn recording and data origin on
  --no-record           leave recording, write history and data origin off
  --no-write-history    leave write history off
  --no-data-origin      leave data origin off
  --kb <path>           use this knowledge store for this run (not saved to the config)
```

In the main window, open the workbench from **System → Decomp workbench...** (Ctrl+Shift+W).
**System → Start recording** (Ctrl+R, the same key as in the workbench) starts and stops recording.

Recording, write history and data origin are on from boot. `--record` on its own still means
recording only. `--no-record` turns all three off.

**Recording needs the interpreter.** When the JIT is on, recording stays off. The Recording
panel says to switch to the interpreter in Config > Emu settings (the game resets).

Functions whose entry actually runs are created automatically, named `sub_` plus the address.
Rename them as you learn what they are. Define function is for code that never ran, an unusual
prologue, an overlay, or a branch that is not a call.

When recording is on:

* The main window title starts with `[REC]`.
* A status bar shows the `REC`/`HIST`/`ORIGIN`/`PAUSED` badges, frames recorded, write
  history use and emulation speed.

## Recording

| Switch | What it adds |
|---|---|
| Recording | Records per physical byte (32 B, in sparse 4 KB pages): exec, read and write counts, DMA counts, last writer PC, frame and CPU, flags and mode. Also call edges, a shadow call stack per CPU, provenance events (card reads, BIOS decompression, DMA, boot loads, CPU copies), overlay loads and evictions, and TCM/WRAM changes. Overlays get separate records per overlay epoch |
| Write history | Writes (frame, CPU, PC, address, size, old and new value), filtered (see below), in memory up to a cap (`Workbench.HistoryCapEntries`, default 4,000,000 entries) or in a history file |
| Data origin | Taint labels that follow data from chosen sources (ROM files by default) through loads, stores, ALU operations, DMA and BIOS decompression. Files inside a game archive get labels of their own (`/vfs.zip:Scripts/config/vehicles.luac`). Values computed on the ARM9 math unit (hardware divider and square root) keep their operands' labels. *Follow table lookups* also passes taint from an address register to the value loaded through it, so decoders such as zlib keep their input's label (see below) |

**Overlay detection.** The recorder learns which overlay occupies a RAM range in two ways:

* **Card read:** a card read of the overlay's file lands in its range.
* **Written, then executed:** code bytes in an overlay range are rewritten, or a savestate is
  loaded. The next instruction run there is then compared with the overlays' ROM images, and
  the one that matches becomes the owner. Compressed overlays (backward LZ) are compared
  with their unpacked image. This covers overlays copied in or unpacked by code or DMA, and
  savestates that bring overlay code never read from the card.

The Overlays panel shows these loads as `load (ran)`.

**Calls.** A jump that leaves `LR` equal to the return address is a call, whatever the
instruction, so indirect calls are recorded with their real target. Each call edge keeps:

* **The overlay of each end.** Code that two overlays load at one address keeps one edge per
  overlay. `calls.get` takes `overlay` and returns `site_overlay` and `callee_overlay`.
* **Whether the call came back** (`returned`: yes, no, or empty when not known). Thumb code uses
  `bl` as a long jump as well, and a `bl` sets `LR` either way, so only a return tells a call
  from a jump. Only a return instruction counts (`bx Rm`, `pop {pc}`, `ldm sp!, {pc}`,
  `ldr pc, [sp], #4`, `mov pc, lr`): a branch or a switch's jump table that lands on the address
  after a `bl` does not.
* **Its kind.** A plain `b` to a known function entry is a tail call, unless it goes back into
  code the running function already passed through, or to an address only ever reached by `bl`
  jumps that never came back. An interrupt is an `interrupt` edge with no call site, and `swi`
  an `exception` edge from the `swi`; neither is a call into the BIOS.

**Write history capacity.** A game writes millions of bytes a second, so history keeps what's
useful and drops the oldest entries when full.

* **Filters** (Recording panel, or `recording.configure`):
  * the bytes of a sequential run past its first 1 KB: copies, fills, unpacking (on by default);
  * writes near the writing CPU's stack pointer (on by default);
  * writes that store the value already there (on by default);
  * optional address ranges to keep (a "main RAM only" switch), and code ranges whose writes to skip.
* **History file.** `history_file_mb`, or the Recording panel's size box, keeps history in a
  memory-mapped file, `workbench/history.dswbh`, at about 32,000 writes per MB. A TrackMania run
  from boot to a race, with main RAM only, needs about 8.6 million entries, so 512 MB holds all
  of it. Session files keep only the newest 4 million entries.
* **When full,** the oldest writes go:
  * a notice says so once;
  * the Recording panel shows the oldest frame still kept;
  * `writes.list` and the recording state report `history_wrapped` and the oldest frame.

**How labels merge.** A label is the set of files a value was computed from. Copying keeps it;
an instruction that combines values with different labels gives the result the union. Each merged
label records how it was first made: the instruction, CPU and frame, the kind of merge (computed,
table lookup, a load of bytes of different origin, the math unit, a copied range, a source added),
and the two labels it joined. The same files joined at two different instructions are two labels,
each with its own history; a merge that adds no new file keeps the label it had.

* The Data origin panel shows the tree under each merged label, three levels deep, with links to
  the instructions; *show how* opens a deeper part in full. `origin.label` returns the tree.
* A running total, one instruction adding to its own result (a loader's position, an allocator's
  total), is shown as one step with its count and the parts it added, oldest first. On the race,
  the 113-file label at `020fc2cc` is "computed at `0207de0c`, 8 steps from frame 1616".
* Recording it costs nothing per instruction: it happens only when a new label is made (about
  6,400 times from boot to the race).

**Following table lookups.** A table lookup turns a labelled index into a loaded value, as in
Huffman decoding. Following every address register would also label everything reached
through pointers that were once advanced by data-derived lengths, so the rule is narrower:

* Each register has an address label, which is what a load through it takes, next to its
  ordinary label.
* When arithmetic combines a pointer-like value (a RAM, IO or VRAM address) with a data operand,
  the result's address label keeps only the data operand's part. A pointer loaded from memory has
  no address label.
* A register counts as a known pointer once it has been used as an address, and so does a
  result derived from one. A known pointer contributes only its address label to arithmetic and
  to stores, so pointers don't spread their history.
* Data that merely looks like an address is never used as one, so it keeps its full label.

**Labelling unpacked files by content.** Taint that passes through an unpacker such as zlib is
lossy: Huffman decoding is a table lookup, and the unpacker keeps state between files that would
carry one file's label into the next. So the workbench doesn't rely on it:

1. When a tracked archived file's bytes are read from the card, the file becomes *pending*.
2. While a file is pending, contiguous write runs (an unpacker's output) are matched against the
   archive's contents whenever a function returns.
3. Bytes that equal a pending file are labelled with exactly that file, overwriting whatever the
   unpacker produced.

This works per chunk, so files the game streams through a small buffer are covered too (Lua
reads `vehicles.luac` this way). The recording state reports the count as `origin_seeded_files`.

Plain NitroFS files in a BIOS compression format are covered the same way: LZ77 (types 0x10 and
0x11, optionally behind an `LZ77` tag), Huffman and run length. The workbench indexes what they
unpack to, so output from a software unpacker that loses taint is still labelled with the file.
Files stored as they are aren't indexed, because copying keeps their labels.
Seeding costs about 15% speed while files are loading, and nothing otherwise.

For a precise answer to "does this come from file X?", track only that file: turn off *All ROM
files*, then use **Add as origin source** in the Game archive panel, or `archive_file` sources in
`recording.configure`. The vehicle parameter test below does this.

### Session files

A session runs from when recording turns on until it turns off or the game closes. At the
end, melonDS writes a `.dswbs` file to `<knowledge store dir>/sessions/`.

* **Contents:** byte records (with each byte's first exec, read, write and DMA frame), how each merged
  data origin label was made, write history, call edges, provenance, overlay, TCM and
  recording events, data origin sources and labels. They also record which function was running
  when each instruction first ran, so `calls.get` can name the function around code reached
  only through branches in old sessions too.
* **Call edges** carry the overlay of each end and whether the call came back (session schema 2).
  Schema 1 files still load; their edges have neither.
* **Format:** zlib-compressed, starting with `DSWBSZ1\0`; uncompressed files are still read.
  Records compress about 60×: 80 s of TrackMania with all switches on is about 2 MB on disk.
* **Import:** the session's function-level evidence goes into the knowledge store
  (`session`, `runtime_evidence`, `asset_link`). A function in an overlay gets its own overlay's
  counts and calls, whichever overlay is loaded when the session ends.
* **"All time" scope:** panels and API calls with `scope: "all"` merge every saved session
  of the same ROM (matched by SHA-1) with the current one. Write history isn't merged: frames,
  values and loaded overlays differ between runs, so it is searched one session at a time.
* **A session's whole write history:** a session file keeps its newest 4 million writes. When
  the session ends with a history file (or with more writes in memory than that), all of its
  history is kept next to it as `<session>.dswbh`.
  * The history file is renamed, not copied, and cut to the part it used. 900 frames of the
    TrackMania race (2.07 million writes) take 63 MB, and ending the session takes half a second.
  * **Disk budget:** `history_keep_mb` (default 2048 MB per game) deletes the oldest `.dswbh`
    files first, never the newest. Session files are never deleted.
  * Saving without stopping (Recording > Save session now, or `recording.configure` with
    `save_session`) copies the history's used part instead, and the session goes on. Two saves in the
    same second and frame get `_2`, `_3` on the name.
  * **Searching:** the Write history panel's *History of* list, or `writes.list` with `session`
    (a file name from `sessions.list`). It searches the `.dswbh`, or the session file's own
    history when there is none, and says which in `history_source`.

To read a session file from Python:

```python
from dswb import session
s = session.load("…/sessions/20261012-131247_f0-4826.dswbs")
s.call_edges(); s.provenance(); s.write_history(); s.byte_record(region, offset)
s.first_frames(region, offset)       # (exec, read, write, DMA), None for never
s.label_merges()                     # label, the two it joined, pc, frame, op
h = session.load_history("…/sessions/20261012-131247_f0-4826.dswbs")   # its .dswbh
len(h), h.wrapped, [e.addr for e in h][:10]                             # oldest first
```

## Game archives (TrackMania DS's VFS)

Many games keep their assets inside one archive in NitroFS. Without help, provenance stops at
"the archive, offset X". The workbench recognises the archive of Firebrand Games' Octane engine,
which TrackMania DS uses, and resolves it down to the archived file.

**Detection.** The resolver switches on when a ROM's NitroFS has `vfs.zip`, `vfs.bin` and
`vfs.dat` in the same directory. When the game loads, melonDS does the following in the
background (about half a second for TrackMania):

1. Reads the ZIP's central directory.
2. Unpacks every file, stored or deflated, and checks its CRC.
3. Rebuilds every path from `vfs.bin` and checks it against the ZIP (all 3,457 agree for
   TrackMania).
4. Indexes the unpacked contents.

| File | Layout |
|---|---|
| `vfs.zip` | A standard ZIP. The game reads only the bytes it needs: the central directory record, the local header, then the data, which it inflates itself |
| `vfs.bin` | `00 00 FF FF 00 00 FF FF`, then one 8-byte record per entry (`vfs.dat`'s first number), then one u32 per file, then the name table (lower-case names). File record: name offset, next sibling (`0xFFFF` = last), `0xFFFF`, file index (= ZIP order). Directory record: name offset, next sibling, first child (= its own index + 1), `0x7FFF`. The u32 per file is the offset of that file's record in the ZIP central directory, so the game can seek straight to it |
| `vfs.dat` | `entries, files, name table size` as text, e.g. `3716, 3457, 64734` |

**What it adds:**

* **Content matching.** The resolver finds runs of RAM bytes that equal part of an archived
  file, whatever path the bytes took, and it works without recording.
  * Files the game patched in place split into several runs.
  * Identical or near-identical files are listed as alternatives. Car colours, for example,
    share most of their bytes.
  * Among alternatives, the file whose bytes the game most recently read from the card wins.
* **Data origin.** Bytes read from inside `vfs.zip` are labelled with the archived file
  (`/vfs.zip:<path>`). The Game archive panel's **Add as origin source** button, or an `archive_file`
  source in `recording.configure`, follows one file only.
* **Card reads.** Reads of `vfs.zip` name the archived file and the part read, e.g.
  `Graphics/Cars/Stadium/11/lod1.nsbmd, compressed data +0x8908`.
* **Loader and unpacker.** Each match carries the card reads of that file's bytes, whose PC is
  the loader, and the code that last wrote the RAM bytes, usually the inflater.
  `calls.get` names the function around such a PC even when it sits in a loop reached only
  through branches. The recorder remembers which function was on the call stack when each
  instruction first ran. For TrackMania that is `02033c48`, zlib's fast inflate loop, called
  from `0203407a`.

**Where to see it:**

* **Data origin and Inspector panels:** an *Archived file* section for the selection.
* **Game archive panel** (View → Project):
  * every archived file, with a filter;
  * **Locate in RAM** (Enter or double-click) shows where the file is in RAM and which code
    read it from the card.
* **Navigate → Locate file in game archive...** (Ctrl+Shift+F) asks for a path and moves the cursor
  and selection to the file's largest copy in RAM.
* **API:**
  * `origin.get` has `archive`, `archive_truncated` and `archive_status` fields (at most 256 runs
    per call; query again from the end of the last run for more);
  * `archive.list` lists and filters the files;
  * `archive.locate` takes a path and returns the file's ROM offset, its RAM copies and its
    card reads (the loader).
* **Knowledge store** (migration 0004):
  * every archived file becomes an `asset` row (`file_id` = the archive, `member` = the path
    inside it, `format` = its extension);
  * session import adds `archive_read` links (card reads of its bytes, with the loader PC);
  * session import adds `content` links (where it was in RAM at the end of the session, with
    the writing code).

Example from a TrackMania race, using the API:

```python
r = c.archive_locate(path="Graphics/Cars/Stadium/11/lod1.nsbmd")
r.member.rom_offset               # 0xddcef8: deflated, 35201 bytes, vfs.bin entry 2387
[(hex(x.addr), x.size) for x in r.ram]    # copies in main RAM
{c.calls_get(l.pc).function for l in r.loads}   # {0x02012f84}: the card loader
```

The resolver lives in `src/frontend/qt_sdl/workbench/GameArchive.cpp`. It is ZIP-based, so another
game's archive needs only a detector and, if it has one, an index parser.

## NARC and SDAT containers

Most DS games keep their graphics and data in NARC files and their sound in SDAT files. The
workbench reads both, next to the game archive above and separately from it, and resolves them
down to the files inside.

* **What is found.** Every NitroFS file that is a NARC or an SDAT, or one packed whole in a BIOS
  compression format (a `.carc`). Files inside them may be packed on their own and may be
  containers themselves; nested containers are read up to 8 levels deep.
* **Names.** A file inside is named `/file.narc:name`:
  * NARC: the names in its file name table. A NARC without names gives the index and an extension
    from the data's magic, e.g. `0007.ncgr`.
  * SDAT: the symbol names, e.g. `SEQ_TITLE.sseq`, else `0003.sseq` by FAT index.
  * Nested files: `sub.narc/0001.ncgr`.
* **ROM offsets.** Files stored in a container that is not packed have ROM offsets, so card reads
  name the deepest file they fetched. Inside a container packed whole (or a packed nested one),
  files are known by content only.
* **Data origin.** Bytes read from a container are labelled with the file inside. Once the game
  unpacks a `.carc` or a packed nested container, the files found inside it take their own labels
  over the container's. **Add as origin source** in the NARC/SDAT files panel, or a `container_file`
  source, tracks one file.
* **Limits.** 512 MB of contents in all; files over 16 MB (streams) are listed and mapped but not
  searched for by content.

Where it shows:

* **NARC/SDAT files panel** (View → Project): the containers, the files of the one selected (name, kind,
  size, form, ROM offset) with a filter, **Locate in RAM**, **Add as origin source** and **Show graphics**,
  the same as the Game archive panel.
* **Data origin and Inspector panels:** a *File in a container* section for the selection.
* **Navigate → Locate file in NARC/SDAT...** asks for a `/file.narc:name` path.
* **API:**
  * `container.list` lists the containers and their files, filtered by container or by name;
  * `container.locate` takes a path and returns the file, its RAM copies and the card reads of its
    bytes;
  * `origin.get` has `containers`, `containers_truncated` and `containers_status` fields.
* **Knowledge store:** every file inside a container is an `asset` row (`file_id` = the
  container, `member` = its name), with `archive_read` and `content` links like the game
  archive's.

TrackMania DS has two SDATs (`/audio/sound_data.sdat`, `/audio/sound_dataNetwork.sdat`), so its
sound banks, wave archives and sequences are named next to `vfs.zip`.

The reader lives in `src/frontend/qt_sdl/workbench/Containers.cpp`.

## Graphics files

The workbench knows the DS graphics formats from public format documentation (GBATEK and the
community's tools), never SDK material:

| Format | What it holds |
|---|---|
| NCLR | Palettes: BGR555, 16 or 256 colours each |
| NCGR | Tiles, 4bpp or 8bpp, as 8x8 characters or a linear bitmap |
| NSCR | A tilemap: tile, H-flip, V-flip and palette per entry |
| NCER | Cells: the OAM entries of each sprite |
| NSBTX, and NSBMD's TEX0 | Textures in every TEX0 format (A3I5, 4, 16 and 256 colours, 4x4 compressed, A5I3, direct) and their palettes |

When a game loads, the workbench finds these files in NitroFS (stored, or packed whole in a BIOS
format), inside NARC and SDAT containers, and inside the game archive. It then finds their copies
in RAM by content.

* **Memory view.** The **File structure** layer (the **Layers** button) tints each part of a RAM
  copy: header, palettes, tiles, map entries, cells and textures. Neighbouring tiles and palettes
  alternate. The tooltip names the part, for example "NCGR CHAR, tile 37, row 3". The **Data objects**
  layer is the struct-field underline, now also switchable.
* **Inspector.** Shows the field at the cursor ("NCLR palette 2, colour 7: 0x7FFF = #FFFFFF") with a
  colour swatch, or the tile drawn with its palette.
* **Graphics panel** (View, or **Show graphics** in the Game archive and NARC/SDAT files panels, or **View as
  graphics** in the Memory view's menu). It shows palettes, tiles, maps, cells and textures.
  * **Palette.** Picks the same-named palette in the same container first, then the colours in
    palette RAM now, then the first palette found.
  * **View as.** Draws any bytes as 2, 4, 8 or 16 bpp tiles or rows.
  * **Clicking a pixel** selects its bytes in RAM, or locates the file when it isn't in RAM.
    Selecting bytes marks the tiles they belong to.
  * **The info line** says where the file is in RAM, VRAM and palette RAM. A RAM source redraws a few
    times a second while the game runs.
  * **Export** saves the image, or every texture, as PNG.

A damaged file is described as far as it makes sense ("understood up to +0x…"); nothing reads past
its end.

## The workbench window

**Default layout:**

* Left: Functions and Bookmarks.
* Centre: Disassembly and Memory.
* Right: Registers, Inspector and Annotations.
* Bottom tabs: Call stack, Cross-references, Write history, Data origin, Breakpoints, Source
  and diff, and Search. The other panels (Call graph, Coverage map, Overlays, Compare setup,
  Decompiler, Game archive, NARC/SDAT files, Graphics, Recording, Getting started) keep their
  place there, closed: open them from the View menu or **Window → Panels...**.

**Window → Reset layout** restores this layout for the current workspace.

Every command has one name and one key, the same in the menu bar, the debug toolbar and the
right-click menus (which show the key next to the command). A right-click command acts on the
row or cell clicked; from the menu bar or a key, it acts on the cursor, or the selection when it
is a range. Names end in "..." only when the command asks for more before it acts.

The debugger's keys stay off the game window's: there, F1 to F9 load savestates, Shift+F1 to
Shift+F9 save them and F12 undoes a load. While a text box in a panel has the focus, the
single-letter keys (N, ;, P, D, G, L, X) type into it.

| Key | Action |
|---|---|
| G | Go to an address, a name, or `pc` |
| Alt+← , Alt+→ (or the mouse's back and forward buttons) | Back / forward |
| L | Lock the disassembly to the PC |
| Ctrl+T | Switch CPU (ARM9 / ARM7); clicking the CPU in the status bar does the same |
| X | Cross-references |
| Ctrl+Shift+F | Locate a file in the game archive |
| N | Rename (function, label, register) |
| ; | Comment (selection or cursor) |
| P | Define or edit function |
| D | Define data (selection or cursor) |
| Ctrl+B | Add bookmark |
| Ctrl+Z / Ctrl+Y | Undo / redo knowledge store edits |
| Ctrl+F5 | Continue (from a pause or a break) |
| Ctrl+Shift+F5 | Break: stop in the debugger at the next instruction |
| F11 / F10 | Step into / step over |
| Ctrl+F10 | Run to cursor |
| Ctrl+F11 | Run one frame |
| Ctrl+F9 | Toggle breakpoint |
| Ctrl+R | Start or stop recording (the same key in the game window) |
| Ctrl+Shift+H / Ctrl+Shift+D | Record write history / data origin |
| Ctrl+Shift+R | Reset game with tracking |
| Ctrl+Shift+B | Build and compare function |
| Ctrl+Shift+E | Open source in editor |
| Ctrl+Shift+I | Import static analysis JSON |

**Debug → Pause** has no key: it pauses the emulator at the end of a frame, while **Break**
stops in the debugger at an instruction, where stepping works. The status bar shows which:
`PAUSED`, or `BREAK:` and the reason in red. **Continue** resumes either.
Annotations can target an address, a range, a function, a data object, a type field, a
register within a function, an overlay or a savestate.

**Authorship.** Every edit is recorded in the change log with its author, and the panels
refresh as soon as anything changes.

* The person's own edits are attributed to the person.
* Agents edit through the API's `kb.*` methods. These call the same code the window uses, and
  are attributed to the agent named in `auth`, tagged with its session (`name#connection`).
* Agent edits are marked `[agent]` in the panels, and the Annotations panel can filter by author.

**Undo.** Each author has its own undo history. Ctrl+Z undoes only the person's changes;
`kb.undo` undoes only that agent's.

### Knowledge store

The knowledge store is an SQLite database in WAL mode, one per game. The default path is
`<melonDS dir>/workbench/<GAMECODE>_<sha1[0:12]>/knowledge.db`; set
`Workbench.KnowledgeStorePath` or use `--kb` to change it.

It holds the following tables:

* modules: arm9/arm7 static, overlays, autoload, and RAM regions for relocated code
* functions, data objects, types, labels, annotations, bookmarks, tasks
* sessions, runtime evidence, build results
* assets (ROM files and files inside game archives), cross-references, decompiler output
* the change log

Migrations live in `src/frontend/qt_sdl/workbench/kb/NNNN_*.sql`. They are applied in order
and recorded in `schema_migrations`. To change the schema, add a new migration; never edit an
applied one.

### Settings

Settings are in **File → Workbench settings**:

| Setting | Meaning |
|---|---|
| Source root | Root of the decompiled C tree |
| Compare command | Shown here; set on the Run tab of **Build → Compare setup**, with this computer's Python, objdiff-cli and compiler paths. Empty: the project's compare script (`compare/compare.py`, copied from the template the first time). The placeholders, the per-module compiler settings (`compare.json`) and the output contract are in [compare/README.md](compare/README.md). Each attempt is kept in `build_result`, and the function's status follows the latest result |
| Editor command | For example `code -g {file}:{line}` |
| Author name | Name shown on your edits |
| API enabled, port, memory write | API settings |

### Static analysis import

**Analysis → Import static analysis** reads a JSON file with any of these keys:

```json
{"tool": "ghidra",
 "functions":  [{"addr": "0x02000800", "size": 64, "name": "Foo", "mode": "arm", "cpu": "arm9", "overlay": -1}],
 "labels":     [{"addr": "0x020e6e5c", "name": "gRaceClock"}],
 "xrefs":      [{"from": "0x02000810", "to": "0x020e6e5c", "kind": "data_ref", "from_overlay": -1, "to_overlay": -1}],
 "decompiled": [{"addr": "0x02000800", "text": "int Foo(void) { ... }"}]}
```

The Decompiler panel shows the imported text with the knowledge store's names substituted.

* **Overlays.** `overlay` on a function or label, and `from_overlay` / `to_overlay` on a reference,
  name the overlay an address is in when several overlays load at that address. Without them
  an address in such a range goes to the first overlay there.
* **Names are overwritten.** A function in the file replaces the store's name, size and mode
  for that function, including a name given by hand. An empty name clears it. A function with
  no `name` key keeps the store's name (a new one gets none, and an automatic `sub_` name goes).
* **Covered ranges.** `"covers": [{"cpu": "arm9", "overlay": -1, "start": "0x02000000", "end": "0x020ffde0"}]`
  says the file is the whole truth for that range (outside every overlay with overlay -1, or of
  one overlay). An import with covered ranges writes as its tool (the author named by `"tool"`,
  `dsd`), whoever runs it, so the next one can tell its own earlier rows apart. In a covered
  range it deletes what it no longer lists: functions the workbench detected on its own,
  functions and labels its earlier import made, and every imported reference that starts
  there, before adding the file's. A function or label a person or agent made or changed is
  never deleted. The window and `kb.import.static_analysis` report how many functions and labels
  were removed (`removed`, `removed_labels`). The changes are on the tool's undo history, not the
  person's: to go back, import the earlier file again.
* **Unnamed functions** are shown as `func_02001234`, or `func_ov003_020ffe00` in an overlay, as
  dsd names them. Those names can be typed into the address box and are understood in
  decompiler text.
* **Importing twice changes nothing.** An entry that is already in the store with the same
  values is left alone and adds nothing to the change log.
* **One step.** The functions, labels and references go in as one transaction and one reload.
  The import's changes are one undo group, but undo keeps only the last 500 changes of an
  author, so a large import cannot be undone whole.
* **Failures are reported.** The window shows how many entries were not imported and why the
  first was not. `kb.import.static_analysis` returns the same as `failed` and `error`.

### dsd configuration and vtables

Two scripts read a [dsd](https://github.com/AetiasHax/ds-decomp) configuration folder
(`symbols.txt`, `relocs.txt`, `delinks.txt`; ARM9 only). Run them with `tools/workbench/python`
on `PYTHONPATH`.

```
python -m dswb.dsd_import path/to/dsd-config -o dsd.json [--apply [--report review.json]]
python -m dswb.dsd_sync path/to/dsd-config [--dry-run]
python -m dswb.dsd_check --dsd path/to/dsd-config --sessions path/to/sessions [-o check.json]
python -m dswb.vtables --dsd path/to/dsd-config --extract path/to/extract [--sessions folder] -o report.json [--labels labels.json]
```

**`dswb.dsd_import`** writes the import file above from dsd's functions, named data and
references, and with `--apply` imports it into the running workbench.

* dsd owns the ARM9 code's names, sizes and modes. A function dsd names gets dsd's name, size
  and mode. A function dsd has not named (`func_02001234`, `func_ov003_020ffe00`) gets dsd's size
  and mode and keeps the store's name. Data dsd has not named is left out.
* Each dsd module is a covered range, so a re-import after editing `symbols.txt` keeps the store
  in step: a function moved, merged or deleted in dsd goes from the store too. ARM7 code is
  never touched.
* Before importing, `--apply` reviews the running store and prints (with `--report`, also as
  JSON): functions of a person or agent whose name, size or mode the import changes; names that
  exist only in the workbench, kept (`dswb.dsd_sync` writes them into `symbols.txt`); functions of
  a person or agent that dsd does not list, kept as conflicts; and each function the import will
  remove.

**`dswb.dsd_sync`** writes the names given in the workbench into dsd-config, so they reach the
build and git. Run it with the workbench open on the game (`--api`); `--dry-run` only shows what it
would do.

* **What it writes:** the names a person or an agent gave to functions, labels and data in the
  store. Generated names (`sub_`, `func_`, `vt_`...) and the dsd import's own rows are not names.
* **Where:** only over a dsd default name (`func_02001234`, `data_ov003_02121c68`, `bss_...`), or
  over a name it wrote before, so a name can be changed again from the workbench. A name dsd has
  from elsewhere (a matcher, a script, by hand) stays; a different workbench name for it is
  listed as a conflict, and the next import puts dsd's name back in the workbench. A name must be
  a valid symbol and unused in every module; an address with no dsd symbol of that kind is
  listed, for a person to add.
* **The names file:** `workbench_names.tsv` in the folder that holds dsd-config records every
  name written (module, address, kind, name). When dsd-config is made again from scratch, the
  sync, or a project script reading the file, puts them back.
* **The build check:** when the project has a `build.ninja`, it runs `ninja` and `ninja check`.
  If either fails, every file is put back and nothing is imported.
* **Then** it imports dsd-config into the workbench (as `dsd_import --apply`).
* Review the change with `git diff` and commit dsd-config and the names file. It does not run
  by itself on every rename: dsd-config is read by the build and written by other tools too, so
  writing it is a step a person takes, with the build checked.
* TrackMania DS, on a copy of the project: three names (a main function, an overlay 3 function,
  a data symbol) written in about 5 s with the build check, `ninja check` still OK; a rename of
  `OS_Halt` and a name dsd already uses were refused; a second run wrote nothing; after
  dsd-config was made again, the project's `windows/workbench_names.py --write` put them back.

**`dswb.dsd_check`** lists where the recordings disagree with dsd's function list, for a person
to look at before `symbols.txt` changes:

* **came back:** a call into the middle of a dsd function that returned to its call site. A
  function probably starts there.
* **jumped between:** a `bl` that never came back, from one dsd function into the middle of
  another. Thumb code uses `bl` for long jumps, so this is a jump, and the two pieces are
  probably one function. A `bl` jump that stays inside one function is normal and only counted.
* **tail between:** the same with a branch the recorder took for a tail call (weaker).
* **unknown:** calls into the middle from recordings made before calls were marked as
  returning.
* **called outside / ran outside:** call targets, and code that ran, in no dsd function.

Whether a call came back is recorded on each call edge (`returned` in `calls.get`: yes, no, or
empty when not known). Only a return instruction counts (`bx`, `pop {pc}`, `ldm sp!, {pc}`,
`ldr pc, [sp], #4`, `mov pc, lr`): a branch or a switch's jump table that lands on the address
after a `bl` does not.

On TrackMania DS, one race: 3,121 call targets start a dsd function and 16 jumps stay inside
one. The one finding: `func_0203bfaa` jumps with `bl` into `luaV_execute+0x4a`. The code agrees:
dsd splits Lua's interpreter loop into four pieces (`luaV_execute`, `func_0203bc58`,
`func_0203bfaa`, `func_0203c33e`). It is one function from `0203b7d8` to `0203c3e0` (0xc08
bytes): `push {r4-r7, lr}; sub sp, #92` at the start, `add sp, #92; pop {r4-r7, pc}` at the end,
the three later pieces have no prologue and are never called. With that change in a copy of
`symbols.txt`, a re-import removed the three pieces and the check found nothing.
* Left out and listed: references dsd could not place in one overlay (`overlays(2,3)`), and a
  second label at an address.
* TrackMania DS: 7,393 functions (1,756 named), 183 labels and 41,065 references import in
  about a second. Into the store as it was on 2026-11-20, the review listed two names of
  `matcher-agent-1` that dsd replaces (`OS_WaitIrq` at `02009d14` is `OS_LoadContext`;
  `OS_WaitForInterruptHalt` is `OS_Halt`) and one of its functions dsd does not have
  (`02009b8c`), and 31 detected functions were removed.

**`dswb.vtables`** finds C++ vtables and ties them to what saved sessions saw. The report is JSON.

* **Tables.** A run of words in `.data` or `.rodata` that dsd marks as pointers to function
  starts. A vtable has two zero words in front of it; a candidate has that header and two or
  more slots. Each table lists its slots, the functions whose literal pool holds its address
  (constructors and destructors), and the tables that share slots with it.
* **Call sites.** Each `blx`/`bx` to a register among the sessions' call edges, with its
  targets. A site is tied to a table slot in two ways. *Decoded:* the instructions before the
  call load the register from `[reg, #offset]`, and a table with the header holds the target at
  that slot. *Readers:* a slot holding the target was last read by code up to 24 bytes before
  the call.
* **Stores and reads.** The code addresses that wrote a table's address into memory (from write
  history), and each slot's read count and reader addresses.
* **Overlays.** A call edge names its overlay (sessions of schema 2). For an older session, an
  edge in code two overlays share goes to the overlay that ran that address in that session,
  when only one did.
* `--labels` writes `vt_<address>` labels for the candidates dsd has not named, as an import file.
* TrackMania DS, 19 sessions: 390 candidates in the main module, 315 indirect call sites, 267
  with a target in a table; 257 tied by decoding, 203 by readers, 265 either way.

## API

**Transport:** JSON-RPC 2.0 over TCP on 127.0.0.1, one JSON object per line. There are two
endpoints:

* **read** (default port 47800, set in Workbench settings or with `--api-port`) for queries;
* **control** (port + 1) for anything that changes emulator state.

**Authentication:** on start, melonDS writes `<melonDS dir>/workbench/api.json` with the
ports and a per-launch token. The first request on each connection must be
`auth {"token": ...}`.

**Shared control:** buttons the person holds always win over API input.

**Versioning:** `status.get` reports the API and schema versions.

| Read endpoint | |
|---|---|
| `status.get` | Emulator, recording and CPU state (the same state the `[REC]` indicators show) |
| `memory.read` | Range by virtual address, optional typed decode |
| `metadata.get` | Byte records aggregated per byte, instruction or block; `scope` session/all. Up to 65,536 entries per call, with `truncated` and `next_addr`. Each entry has the first frame its bytes were executed, read, written and DMA'd this session (`first_exec`, `first_read`, `first_write`, `first_dma`, `first_kinds`, and the earliest as `first_frame`) |
| `writes.list` | Write history for an address or range, by frame window, CPU and writer PC range (`size = 0` searches all memory). Pages of up to 100,000, newest first via `skip`, with the `total`. Reports `history_wrapped` and `oldest_frame`. With `session`, a saved session's history instead of the running one's (`history_source` says from which file) |
| `sessions.list` | Saved sessions of this game, newest first: frames, size, and whether a history file kept all of its writes |
| `coverage.get` | Executed / read / written / DMA ranges, optionally only outside known functions. With `kind = write` and `writer_from` / `writer_to`, it returns the bytes a function wrote last; this needs no write history, so it works when history has wrapped |
| `analysis.get` | The unreached-code snapshot for one CPU: `start`, `end`, `kind` (`arm`, `thumb`, `data`) and `why`. `addr` and `size` limit it; `size` 0 is the whole snapshot. Paged with `limit`, `truncated` and `next_addr` |
| `calls.get` | Observed callers and callees (infers the function entry if unknown). A branch to a known function entry (`b func`, conditional or not) counts as a call of kind `tail_call`. Entries are known from observed calls and from the knowledge store's functions: the callee's code is credited to the callee, and the Call graph marks the edge |
| `callstack.get` | Shadow call stack |
| `origin.get` | Provenance chain (ROM file and offset → loads → copies), taint labels, and the archived files the bytes equal. A merged label's run also says how it was made (`how`, `how_pc`, `how_frame`, `how_steps`) |
| `origin.label` | How a label naming several files was made, as a tree: for each merge the instruction, CPU, frame and kind (computed, table lookup, load of bytes of different origin, math unit, copy, source added), and what it joined. A running total (one instruction adding to its own result) is one node with its step count and the parts in order |
| `origin.ranges` | Where a source's data went: the RAM ranges whose label includes it (by source name, or the end of the name such as `Scripts/config/vehicles.luac`) |
| `screen.capture` | Both screens as PNG (software and OpenGL renderers) |
| `archive.list`, `archive.locate` | Files of a detected game archive; where one is in RAM and which code loaded it |
| `asset.list`, `asset.describe`, `asset.render` | The game's graphics files; what each range of one is (or of the RAM copy holding an address), with its RAM copies and where its parts are in VRAM; a PNG of a palette, tiles, a map, a cell or a texture, or of raw bytes viewed as tiles |
| `container.list`, `container.locate` | NARC and SDAT containers and the files inside them, nested ones too; where one is in RAM and which code loaded it |
| `disasm.get` | Disassembly decoded by each line's classification. Each line has `classification` (`arm`, `thumb`, `mixed`, `data`, `unknown`), `confidence`, `conflict` and `why`; `metadata.get` entries have the first two. With `overlay`, an overlay epoch: where another overlay is loaded, that overlay's own records and its bytes from the ROM (unpacked when compressed), as the Overlays panel's *View this epoch* shows it |
| `xrefs.get` | References to an address, as the Cross-references panel lists them: imported static references, then observed calls, writes (with counts when write history is on), reads and literal-pool loads, each with the function it is in. `overlay` picks the imported references and names in overlay code; observed references are by address |
| `overlays.list` | The ROM's overlay table (RAM range, file, compressed, which are loaded now) and the recorded load events, newest first |
| `search.bytes`, `search.string` | Find a byte pattern (`??` wildcards) or text (ASCII/Latin-1, Shift-JIS or UTF-16LE) in main RAM, or also shared WRAM, DTCM and ITCM |
| `search.scan` | Value scan: a first scan (u8, u16, u32, s32 or fx32; equal, not equal, greater, less, or unknown), then narrow it down across pauses with `scan_id` (changed, unchanged, increased, decreased, or a value). Scans belong to the connection, which keeps its newest 8 |
| `breakpoints.list`, `recording.get` | Breakpoints and hits; recording state and data origin sources |
| `kb.list` | The knowledge store's lists, one `kind` per call: `functions`, `labels`, `data_objects`, `types` (with their resolved fields), `annotations`, `bookmarks`, `decode_overrides`, `modules`. Filters as in the panels: `filter` (name or address), `author` (`people`, `agents` or a name), `cpu`, `module`, an address range (`addr`, `end`; `addr` alone: what covers that address), `id`; for functions `status`, for annotations `target_kind` and `tag`, for decode overrides `conflicts_only`. Paged with `offset` and `limit` (`limit = 0` only counts). Functions carry `exec_count` (the Calls column), decode overrides their `conflict` with the recording |
| `kb.function.get` | One function (by address or `id`): its fields, the Compare attempts newest first, the imported decompiler output as written and with knowledge-store names, its annotations, and its source file's text. The text is only given for files inside the source root set in Workbench settings (`source_status` says why not), so a `source_file` pointing elsewhere can't be used to read other files |

| Control endpoint | |
|---|---|
| `game.boot`, `game.reset` | Boot a ROM, reset |
| `execution.control` | `pause`, `resume`, `step`, `step_over`, `break`, `run_frames` (then pause). `step_over` runs until a call returns and requires `count` 1 |
| `analysis.run`, `analysis.cancel`, `analysis.clear` | While paused or in a break, mark code the recording has not reached. The run replies with the status-bar summary. Cancel keeps the previous snapshot; clear removes the marks and is refused while a walk is running |
| `input.send` | Hold buttons / touch for N frames; returns the frame number and both screens |
| `state.save`, `state.load` | Savestates |
| `breakpoint.set`, `breakpoint.clear` | Exec / read / write / access breakpoints with conditions |
| `breakpoint.update` | Enable, disable or change the condition of a breakpoint, keeping its hit count (the Breakpoints panel's *Enable / disable* and *Edit condition*) |
| `recording.configure` | Recording, write history and data origin switches and sources in one call; `save_session` saves a session file without stopping |
| `memory.write` | Only with `--api-memory-write` or the setting |
| `kb.function.define`, `kb.rename`, `kb.label.set`, `kb.data.define`, `kb.type.define` | Knowledge-store edits attributed to the agent. `kb.function.define` picks ARM or Thumb from the recording when `mode` is `auto` |
| `kb.annotation.save`, `kb.annotation.delete`, `kb.bookmark.add` | Annotations on any target kind, and bookmarks |
| `kb.decode.set` | Decode a range as `arm`, `thumb` or `data` whatever the recording says, or `clear` that; overlapping overrides are trimmed, and one undo reverses the change |
| `kb.function.update`, `kb.function.delete`, `kb.data.delete`, `kb.label.delete`, `kb.type.delete`, `kb.bookmark.delete` | Change or delete; a deleted function takes its build results, annotations and evidence with it, and one undo restores all of it |
| `kb.build.compare`, `kb.build.record` | Run the configured compare command for a function (as the Source and diff panel's Compare does; `record: false` for a dry run), or record a result from elsewhere. The command must have been approved in the workbench first |
| `build.setup.check`, `build.selftest` | The Compare setup checklist rows, and the toolchain self-test (see [compare/README.md](compare/README.md)) |
| `kb.import.static_analysis` | Import a static analysis export, as Analysis → Import static analysis does |
| `kb.undo`, `kb.redo` | The agent's own changes only |
| `ui.show` | Point the window at an address, a selection, a panel or a CPU, and bring it forward. `addr` moves the cursor (follow-PC turns off, and Back returns to where the person was). `sel_addr` / `sel_size` highlight a range, which write history, data origin, the inspector, cross-references and coverage follow; on its own that leaves the cursor where it is. `panel` is a short name (`disasm`, `memory`, `history`, `origin`, …) or a dock name (`dock_disasm`). `note` is a status-bar message. `cpu` on its own follows that CPU's PC |
| `ui.capture` | PNG of the workbench window, or of one panel (`max_width` defaults to 1600, `0` keeps the native size). It does not raise the window. A minimized window, or a panel that is closed, is an error. The game's own picture stays `screen.capture` |

`kb.changes.list` (read endpoint) returns recent changes, newest first, with author, kind and session.
`kb.list` and `kb.function.get` are on the read endpoint too.

`auth` and `events.subscribe` work on both endpoints. The pushed events are
`breakpoint_hit`, `paused`, `resumed`, `recording_changed`, `frames_done` and `input_done`.

### Python

```python
import sys; sys.path.insert(0, "tools/workbench/python")
from dswb import Client
with Client.connect(agent="my-agent") as c:   # finds api.json (or set DSWB_API_INFO / MELONDS_DIR)
    c.game_boot(path="game.nds")
    c.recording_configure(enabled=True, write_history=True)
    c.input_send(buttons=["a"], frames=30)
    c.breakpoint_set("write", 0x020E6E5C, size=4)
    hit = c.wait_event("breakpoint_hit")
    print(c.writes_list(addr=0x020E6E5C).entries)
    c.kb_label_set(0x020E6E5C, name="gRaceClock")       # attributed to my-agent, undo with c.kb_undo()
```

There is also a CLI: `python -m dswb status.get`,
`python -m dswb memory.read '{"addr": "0x02000000", "size": 32}'`, or
`python -m dswb --session file.dswbs`.

Numbers in parameters are JSON numbers, or strings. An address written as a string is hex, the way the
window reads it: `"0x02000000"`, `"02000000"`, `"$02000000"` and `"02000000h"` are the same address. A
count or size written as a string is decimal unless it starts with `0x`. A number that is not whole, or
does not fit its field, is refused. (API 1.1; 1.0 read `"02000000"` as decimal.)

### Changing the schema

1. Edit `src/workbench/schema/workbench.schema.json`. Bump `schema_version` when a record
   layout changes, and `api_version` when the API changes incompatibly. Older session files
   must still load: add fields at the end of a record, and read its sections with the record
   size the section itself gives (its size over its count), as `CallEdge` is read. Schema 2
   added the overlay of each end to `CallEdge`; a schema 1 edge reads as having none.
2. Build. CMake reruns `wbgen.py`, which regenerates the C++ headers and
   `python/dswb/_generated.py`.
3. Implement the method in `ApiServer.cpp` and, for queries, in `WBQueries.cpp`. Give it a
   panel or a command in the window too.

## Tests

| Command | What it checks |
|---|---|
| `cmake --build build --target wbtests && build/wbtests` | Unit tests: the compare result contract, compare.json, the target object (symbols, relocations), the context header, the graphics formats (exact pixels for every format, part ranges, fuzzed input, the test ROM's files built the same way), condition expressions, disassembler, data-flow decoding, ROM parsing, C type layouts, knowledge store (undo, including changes others built on, change log, migrations, vocabularies, the lists and filters of `kb.list`), archive resolver (ZIP, vfs.bin, matching, locating), container reader (NARC with and without names, SDAT with and without symbols, files packed on their own, NARCs nested three levels and packed whole, damaged input), ROMs and archive indexes with sizes that wrap past 4 GB, data origin across an IRQ, the API server closing its connections |
| `python tools/workbench/compare/test_compare_script.py` | The reference compare script without melonDS: failing, hanging and garbage-writing compilers, missing or old objdiff-cli, flags and the context header reaching the compiler; with devkitARM and objdiff-cli, the self-test and a real mismatch |
| `python tools/workbench/tests/test_compare_gcc.py` | Compare end to end with devkitARM's gcc standing in for mwccarm: the test ROM's own C functions (`testrom/match_src/wb_match.c`) match byte for byte through the target object and context header, `wb_match_wrong.c` does not, dry runs record nothing, `build.setup.check` and `build.selftest`, and failures come back explained |
| `python tools/workbench/tests/ui_compare_setup.py` | The Compare setup checklist from a fresh setup to a passing self-test, with screenshots of the Source and diff panel before and after |
| `python tools/workbench/tests/test_assets.py` | Graphics files on the test ROM: listed, described (parts, RAM copies, VRAM and palette RAM), drawn the same from the ROM and from RAM with the expected pixels for tiles, a map with flips, a cell and every texture format, palette pairing, a damaged file |
| `python tools/workbench/tests/test_assets_game.py --rom game.nds [--out dir]` | A real game's graphics files: every one is described and drawn without an error, and how many were understood whole (TrackMania DS: 2055 files, all whole, 587 textures). `--out` writes the images to look at |
| `python tools/workbench/tests/ui_graphics.py` | The Graphics panel shows a file, and clicking a tile selects its bytes in RAM. Saves `build/ui_screens/graphics.png` |
| `python tools/workbench/tests/test_dsd_check.py` | `dswb.dsd_check` without melonDS: session files built by the test, with a call target of each kind it sorts (a call into the middle that came back, a `bl` jump between functions and inside one, tail calls, an older recording) and code that ran outside every function |
| `python tools/workbench/tests/test_dsd_sync.py` | `dswb.dsd_sync` without melonDS: names written over dsd's default names (function, data, bss, an overlay) with the rest of each line and its line ends kept, conflicts and invalid or used names refused, the names file, renaming a name it wrote, putting names back after dsd-config is made again, and a failing build check putting every file back |
| `python tools/workbench/tests/test_dsd_import.py` | `dswb.dsd` and `dswb.dsd_import` without melonDS: a small dsd configuration with ITCM and two overlays at one address becomes the import file, with what was left out |
| `python tools/workbench/tests/test_vtables.py` | `dswb.vtables` without melonDS: two related vtables and a callback table are told apart, each instruction form of an indirect call is decoded, and call sites are tied to tables by decoding and by readers |
| `python tools/workbench/tests/test_client.py` | The Python client without melonDS: waiting for events, and timing out with `TimeoutError` |
| `python tools/workbench/tests/test_recording.py` | Test ROM: exact counts, modes, call edges, write history, provenance through card, BIOS LZ77 and DMA, taint into struct fields including a table lookup, sessions, all-time scope, API endpoint separation, the test ROM's own Octane VFS (detection, origin, locate, knowledge store links, a file unpacked through a lookup table labelled by content), its NARC, packed NARC and SDAT (names, labels of files read, unpacked by the BIOS, nested two levels down and inside a NARC packed whole, locate, knowledge store links), a plain LZ77 file unpacked in software, and on the ARM7 a tail call and an archived file copied as is and through a lookup table |
| `python tools/workbench/tests/ui_person_flow.py` | Phase 3: a person names a function, its data and a register, comments, bookmarks, defines data, undoes, compares, imports and locates an archived file using only keyboard shortcuts in the window |
| `python tools/workbench/tests/ui_settings.py` | The settings dialog shows an API started with `--api` as running, on its port: changing another API option keeps it running there, and only the Enable box turns it off. Sends keystrokes |
| `python tools/workbench/tests/test_quit.py` | Quitting: closing the main window exits with code 0 with the workbench window open, while recording, and with API clients connected. Opens and closes windows |
| `python tools/workbench/tests/test_prereq.py` | Prerequisites: `--data-origin` records from boot with write history off; `--no-record` leaves all three off, and turning data origin on (Ctrl+Shift+D) starts recording |
| `python tools/workbench/tests/ui_screens.py` | Screenshots for review in `build/ui_screens/index.html`: each workspace, each panel with nothing recorded, and the coverage map, mode gutter and data origin with data |
| `python tools/workbench/tests/ui_point.py` | `ui.show` and `ui.capture`: the read endpoint refuses them, a bad call does not open the window, the cursor and selection come back as the window has them, a closed panel is refused, and the PNG is a real picture of the window and of one panel. Also called from `test_recording.py` |
| `python tools/workbench/tests/agent_scenario.py --rom game.nds` | Phase 2/4: boots TrackMania DS through the API, plays to a race with recording on at full speed, finds the race clock and the code that writes it, and checks that call edges in and out of overlay code name their overlay |
| `… agent_scenario.py --struct --rom game.nds` | Phase 8: tracks only `Scripts/config/vehicles.luac` from boot, without following table lookups (its unpacked chunks are labelled by content). In the race, it finds the loader from data origin alone (`origin.ranges`): outside copies of archived files, the densest block of the file's data words that was written once while loading rather than every frame. That is `020a9074`, which `load_vehicle_physics_parameters` reaches. The test checks that the struct it wrote carries the file's label (93% of words) and names the file's card reads and loader. It also checks that words equal to other archived files don't carry the label (0% false positives) |
| `… agent_scenario.py --assets --rom game.nds` | Phase 7: resolves the largest 3D/VRAM DMA sources to files in `vfs.zip` with their loader and unpacker, locates a car model (ROM offset, RAM, loader function) and checks the knowledge store links |
| `python tools/workbench/tests/bench_speed.py` | Uncapped frame rate with each switch |

Build the test ROM with devkitPro:

```
cd tools/workbench/testrom
python make_assets.py      # payload.bin, packed.lz, a small vfs.zip / vfs.bin / vfs.dat, containers, gfx/
make
```

The UI test sends keystrokes to the window, so do not use the mouse or keyboard while it runs.

### Measured speed

TrackMania DS, uncapped, software renderer:

| Scene | Upstream | Recording off | Recording on | + history | + data origin |
|---|---|---|---|---|---|
| Menu | 583 fps | 578 fps | 380 fps | 371 fps | 313 fps |
| 3D race | — | 265 fps | 125 fps | 125 fps | 95 fps |

All of these are above the 60 fps the console runs at. Loading a race with data origin on every
ROM file (content labelling included) runs at about 62 fps uncapped.

## Known limitations

* **Data origin labels can merge.** A value computed from several files carries all of them. On
  the TrackMania race with every ROM file as a source, 96.4% of labelled main RAM names one file
  and 3.6% names several (up to 122 with *Follow table lookups* on, 73 without). Each merged label
  says how it was made (see **How labels merge**), so a running total that touched every file
  can be told from a value that really combines two. Tracking the one file in question still
  gives the sharpest answer.
* **Only the Octane VFS is recognised**, which is all TrackMania needs.
* **Not everything the game DMAs is an archived file.** In a race, about 80% of the largest 3D
  source range matches archived files. The rest is data the game builds at run time: pointer
  tables, structures and fill. For those bytes, Data origin names the writing code instead.
  Only a few hundred bytes are archived data the game patched after unpacking; they show as
  a file split into runs.
* **Components 4–6 are not implemented here:** the full knowledge store pipelines, the build
  server and the MCP servers. The knowledge store schema is in
  place, the compare step is a configurable external command, and the MCP servers would wrap
  the API above.
