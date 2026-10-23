# Workbench UI requirements: flexible workspace, clear prerequisites, coverage map, code/data clarity

Scope: the Qt decomp workbench in `src/frontend/qt_sdl/workbench/`. Each requirement has an ID, the
required behaviour, and acceptance criteria (AC). Section 0 records what exists today, so the work
extends the current code instead of rebuilding it.

## 0. Current state

| Area | Today | Where |
|---|---|---|
| Panels | 21 panels, all `QDockWidget`s, with no central widget. They move, float and close; nested docking is on. | `WorkbenchWindow::BuildPanels`, `AddDock` |
| Show/hide | The View menu lists every dock's `toggleViewAction()` plus "Reset layout". A dock "shown" this way can stay hidden behind another tab. | `WorkbenchWindow::BuildMenus` |
| Persistence | One `saveState()` blob in config (`Workbench.WindowState`), saved only in `closeEvent`. No named layouts. | `WorkbenchWindow::closeEvent` |
| Game screen | `ScreenPanel` (native or OpenGL) is the central widget of the separate `MainWindow`. | `Window.cpp` (`setCentralWidget(panel)`), `Screen.h` |
| Write history, data origin | Toggle at run time with no restart. Silently ignored unless Recording is on (`if (on && !WB.Rec.Enabled()) return;`); their menu items are greyed out until then. They capture from the moment they are turned on. No CLI flags. | `WBHost::SetWriteHistory`, `SetDataOrigin` |
| Write history capacity | A ring of 4M entries (~32 B each); the oldest entries drop silently. Filters for bulk runs, stack writes, unchanged writes and include/exclude ranges are being added (GAPS.md item 3). | `Recorder::HistoryWanted` |
| Savestate load | Overlay ownership is re-checked against the ROM ("written, then executed"). Byte records, call edges and data-origin labels are kept as they were. | `Workbench::OnStateLoaded` |
| Call graph | Built from recorded call edges (this session plus saved sessions). Needs Recording only. Shows an empty tree with no explanation when there's no data. | `CallGraphPanel` |
| Coverage map | 512 px wide; bytes per pixel set by a spinbox; clicking jumps to the address. No zoom, no hover tooltip (mouse tracking is on but unused), no legend. | `CoveragePanel` |
| ARM/Thumb/Data | Mode comes from executed-byte flags, then the knowledge-store function's mode (drawn dimmed), else `.word`. A literal pool counts as data. Nothing says *why* a row is decoded as it is, and data objects don't affect decoding. | `ViewPanels.cpp` (disassembly rows) |

## 1. Decisions

| # | Question | Decision |
|---|---|---|
| D-1 | Docking framework | **Qt Advanced Docking System (ADS)**, LGPL-2.1, Qt 6. It replaces `QDockWidget`. It provides auto-hide side bars, drop-zone overlays, per-area tab menus and saved "perspectives", which back the workspaces. |
| D-2 | Write history full | **Drop the oldest entries**, warn the first time it happens, keep a visible indicator, and offer a larger history: a bigger in-memory cap or a disk-backed history file (HC-3). |
| D-3 | Savestate load and data origin | **Proposed, needs confirming:** clear the labels, because they describe memory the load replaced. Then rebuild what can be proven by content: static modules and overlays that still equal the ROM, and archived files that still equal their unpacked bytes. Mark the session with "state loaded at frame N". Labels are never kept as stale: a wrong origin is worse than none. See SS-1. |
| D-4 | Enabling write history or data origin while Recording is off | **Start Recording automatically**, with a notice saying so. |

## 2. Workspace and layout

### WS-0 Move to Qt Advanced Docking System
- Bring in ADS as a git submodule under `src/frontend/qt_sdl/ADS` (or as a system package if MSYS2 provides one), built by CMake for Qt 6. It is LGPL-2.1, so linking it dynamically keeps melonDS GPLv3-clean.
- Replace `AddDock` with a factory that creates an `ads::CDockWidget` per panel. Keep the object names (`dock_disasm`, `dock_memory`, …) so the env test hooks (`DSWB_SHOW_PANEL`, `DSWB_GOTO`, `DSWB_LOCATE`) and screenshot tests keep working.
- Turn on: focus highlighting, drop overlays, tab close buttons, the tab menu button, and auto-hide (pin/unpin to a side bar).
- **AC:** All 21 panels open, dock, float, tab, auto-hide and close. Every existing test (UI flow, screenshots) passes unchanged.

### WS-1 The game screen as a panel
- Add a **Game screen** panel showing the live DS screens. It takes keyboard, touch and mouse input exactly like the main window.
- Two modes, chosen in View and in Workbench settings:
  - **Separate window**: today's behaviour.
  - **Embedded**: the screen lives in the workbench and the standalone `MainWindow` hides. Its File/Config/System menus move to an **Emulator** menu in the workbench, along with a small toolbar (pause, reset, frame step, savestate slots, screen layout).
- The panel offers the main window's screen options: layout (top/bottom, side by side, single), screen sizing, aspect ratio, integer scaling and rotation.
- It can float or sit on a second monitor like any panel.
- **Spike first:** `ScreenPanelGL` owns a native GL surface and context, and re-parenting it between windows can break the context. Compare (a) a second `ScreenPanel` bound to the same `EmuInstance` framebuffer with (b) rendering to a shared texture that the panel draws. Pick one before building the rest.
- **AC:**
  - In embedded mode the game runs, renders and takes input with both the OpenGL and software renderers.
  - Closing the game panel only stops drawing; emulation continues and the status bar shows "game hidden".
  - Restarting the app restores the chosen mode.

### WS-2 Workspaces
- Add a **Window → Workspaces** menu built on ADS perspectives. A workspace saves:
  - panel visibility, position, size, tab order, floating geometry and auto-hide state;
  - the game-screen mode;
  - the main window's geometry.
- Built-in workspaces:

| Workspace | Panels |
|---|---|
| Decomp | Functions, Disassembly, Decompiler, Source and diff, Cross-references, Inspector |
| Debug | Game screen, Disassembly, Registers, Call stack, Breakpoints, Memory |
| Data tracing | Game screen, Memory, Write history, Data origin, Archive, Inspector |
| Coverage | Game screen, Coverage map, Functions (unknown code), Overlays |
| Minimal | Game screen, Disassembly |

- The user can **Save current as…**, **Rename**, **Delete**, **Reset to default**, and **Export/Import** a workspace as JSON to share it.
- Switching workspaces is instant and needs no restart. Ctrl+Alt+1…9 switch to the first nine.
- Workspaces are stored per user in config. A project can also carry its own `workspaces.json` in the project folder.
- **AC:** Create a workspace, rearrange it, restart, switch away and back: the layout is identical each time. "Reset layout" resets only the current workspace.

### WS-3 Finding and showing panels
- Group the View menu:

| Group | Panels |
|---|---|
| Code | Disassembly, Decompiler, Source and diff, Functions, Cross-references, Call graph |
| Runtime | Registers, Call stack, Breakpoints, Memory, Inspector |
| Tracing | Write history, Data origin, Coverage map, Recording |
| Project | Bookmarks, Annotations, Overlays, Archive, Search |
| Emulator | Game screen |

- A **Panels…** palette (Ctrl+Shift+P) finds and opens a panel by typing its name or description.
- Opening a panel that sits behind another tab raises it and focuses it.
- Each panel's tab and title bar has a menu: Float, Dock left/right/top/bottom, Auto-hide, Close, Close other tabs.
- **AC:** From the keyboard, every panel can be found and focused within two actions.

### WS-4 Sizing and docking ergonomics
- Hiding and re-showing a panel restores its previous size.
- Every panel has a minimum size, so it can't collapse to 0 px and get lost.
- Dragging a panel shows drop zones (ADS overlays).
- Auto-hide side bars as in IntelliJ: a pinned panel slides out on hover or click and hides again on focus loss.
- **AC:** Hiding the Memory panel and showing it again restores its height. A panel can be auto-hidden to each of the four edges and brought back.

### WS-5 Saving the layout reliably
- Save the layout when it changes (debounced, about 1 s), not only on close, so a crash doesn't lose it.
- Version the saved state. If a restore fails, for example because a panel was added, fall back to the workspace's default without leaving panels off-screen or zero-sized.
- Floating panels that end up off every screen (a monitor was unplugged) move back onto a visible one.
- **AC:** Kill the process after changing the layout, restart: the change is there. Restore a layout saved with fewer panels: the new panels appear in their default place.

## 3. Prerequisites and in-context guidance

Users can't tell today which features need Recording, Write history, Data origin, the interpreter
(JIT off) or a restart. None needs a restart, but each records only from the moment it is turned
on. The UI must say this wherever it matters.

### PR-1 One table of prerequisites
One table in code maps each feature to what it needs and what turning it on late loses. Banners,
tooltips, disabled-menu explanations, `status.get` and API errors all read from it.

| Feature or panel | Needs | Restart? | Covers |
|---|---|---|---|
| Recording: coverage, counts, call edges, call stack | A game loaded; the interpreter (JIT off) | No | From when it's turned on |
| Write history | Recording (started automatically, D-4) | No | From when it's turned on, up to the capacity (HC) |
| Data origin | Recording (started automatically, D-4) | No | From when it's turned on. Data loaded earlier has no origin, except files recognised by content (archived files, unchanged ROM modules) |
| Card read, DMA and decompression provenance | Recording | No | From when Recording is on |
| Call graph | Recording (this session) or saved sessions (all time) | No | Calls made while recording |
| Call stack | Recording | No | Calls made after it started |
| Coverage map, "all time" | Saved sessions on disk | No | Every saved session of this ROM |
| Archive panel, archived-file matching | A detected game archive (TrackMania's VFS) | No | Always, by content; card reads only while recording |
| Breakpoints, stepping | The interpreter (JIT off) | No | n/a |

### PR-2 Empty-state banners
- Every dependent panel shows a banner instead of an empty table when something is missing. The banner has:
  1. what is missing ("Call graph needs Recording");
  2. a button that fixes it ("Start recording", "Turn on write history", "Turn off the JIT");
  3. one line on what the data will cover ("Only calls made after recording starts appear here").
- Panels: Call graph, Call stack, Coverage map, Write history, Data origin, Functions ("executed code outside functions"), the Disassembly heat column, Overlays (events), Breakpoints (JIT on), and Archive (no archive detected).
- The Write history header text "write history is off; showing the last writer only" becomes a banner with a button.
- **AC:** On a fresh launch with no flags, every listed panel shows a banner with a working button. After pressing it, the panel fills with data without going to another menu.

### PR-3 Turning on a feature turns on what it needs
- Turning on Write history or Data origin while Recording is off **starts Recording too** (D-4). A status-bar notice says so: "Recording started so that write history can run."
- They can be armed before a game boots, in Workbench settings and in the Recording panel: "When recording starts, also turn on: ☐ Write history ☐ Data origin".
- New CLI flags `--write-history` and `--data-origin`; each implies `--record`. The API's `recording.configure` already accepts both. It also starts Recording, and reports that in its result.
- **AC:** `melonDS.exe --data-origin game.nds` boots with Recording and Data origin on from frame 0.

### PR-4 Reset the game with tracking on
- **Recording → Reset game with tracking…** opens a dialog:
  - checkboxes for Recording, Write history and Data origin;
  - the write-history capacity (HC-3);
  - keep or close the current session.

  It resets the console with those armed, so boot-time loads are captured.
- Every banner that says data was missed because a feature started late links here.

### PR-5 Status bar and tooltips
- The REC, HIST and ORIGIN badges are always visible: grey when off, coloured when on. Clicking one toggles it. Its tooltip says what it does and what it needs.
- HIST and ORIGIN show "since frame N". HIST also shows its fill level and turns amber once it wraps (HC-2).
- Every disabled menu item has a tooltip saying why ("Needs Recording", "Disabled while the JIT is on").

### PR-6 Getting started and help
- A dismissable **Getting started** panel says in plain language what Recording, Write history and Data origin give you, and which panels use them.
- **Help → Feature requirements** shows the PR-1 table with live on/off state.

## 4. Write history capacity (new)

### HC-1 Fewer wasted entries
Filters decide which writes reach history. All can be configured in the Recording panel, Workbench
settings and `recording.configure`; the defaults are marked.
- **Bulk runs:** skip a sequential run (copy, fill, unpacking) past its first 1 KB. On by default.
- **Stack:** skip writes near the writing CPU's stack pointer (spills, locals). On by default.
- **Unchanged:** skip writes that store the value already there. On by default.
- **Address ranges:** keep only writes to chosen ranges. A "main RAM only" preset is one click.
- **Writers:** skip writes by chosen code ranges. "Skip this function's writes" is in the Disassembly and Functions context menus.
- **AC:** The Recording panel shows how many writes the filters skipped, and why.

### HC-2 When history is full
- History drops the oldest entries (D-2).
- The first time it wraps in a session, a notice explains it: "Write history is full: the oldest writes (before frame N) are being dropped."
- The notice offers **Increase capacity…** (HC-3) and **Filter writes…** (HC-1).
- The HIST badge turns amber and shows "since frame N", the oldest frame still kept.
- Write history query results say when they are incomplete: "history starts at frame N; older writes were dropped". The API returns this as `oldest_frame` and `wrapped`.
- **AC:** Fill history in a TrackMania session: the notice appears once, the badge shows the oldest frame, and `writes.list` reports `wrapped`.

### HC-3 A larger history
- **In memory:** the capacity is set in entries or MB, up to available RAM, with the memory cost shown before applying.
- **Disk-backed history file:** history is written to a file in the project's sessions folder, for example `history-<session>.dswbh`, with a size limit set by the user (1 GB is about 33M entries). The oldest entries drop once the file reaches its limit. Queries read from the file through the same API.
- Changing capacity takes effect when the next recording session starts. The dialog offers to start one now, or to "Reset game with tracking" (PR-4).
- Session files include as much history as fits in their own limit, which is configurable.
- **AC:** With a 2 GB history file, a TrackMania session from boot to a finished race keeps every filtered write, and the race load's writes can be queried after the race. Query time stays under a second for a function's writes.
- **Decided (open question below):** one history per session, no search across sessions. When a session ends, its history file is renamed to `<session>.dswbh` next to the session file (cut to its used part, with a trailer) instead of being shared and overwritten; history in memory is written out only when it exceeds what the session file keeps. A disk budget (`history_keep_mb`, default 2 GB per game) removes the oldest history files first. The Write history panel's *History of* list and `writes.list`'s `session` search one saved session; `sessions.list` lists them. All-time search isn't built: frames, values and loaded overlays differ between runs, and the questions that span runs are answered by the merged byte records (`coverage.get` with a writer range, last writers, data origin).

## 5. Savestates

### SS-1 Loading a savestate during recording (D-3, proposed)
- The session records a "state loaded at frame N" event. Panels that show history (Write history, Data origin, Coverage) mark it on their timelines.
- **Byte records and call edges are kept.** They describe what ran, and still count.
- **Data-origin labels are cleared**, then rebuilt from what can be proven by content:
  - static modules and overlays whose bytes still equal the ROM;
  - archived files whose unpacked bytes are found in RAM by the archive resolver.

  Everything else starts unlabelled.
- The Data origin panel shows a banner for bytes with no label after a load: "Origin unknown: this memory came from a savestate loaded at frame N."
- Overlay ownership is re-checked against the ROM (already done).
- **AC:** Load a race savestate with data origin on. Unchanged ROM code and archived models are labelled with their files at once, other memory has no label, and the banner explains why.

## 6. Coverage map

The four requirements below are built in that order: zoom and hover give the biggest improvement
for the effort, the click actions reuse what the window already has (`GoTo`, `DefineFunctionAt`), and
the legend comes last.

### CV-1 Zoom and pan
- Ctrl + mouse wheel and pinch zoom toward the cursor; +, − and Fit buttons do the same.
- Zooming changes the bytes per pixel, down to 1 byte per pixel. Beyond that, a byte is drawn as an N×N cell. The bytes-per-pixel spinbox stays in sync.
- Pan by dragging or with the scroll bars. Each region remembers its zoom and position.
- The map fills the panel's width instead of a fixed 512 px. An option makes row widths a power of two, so address alignment is easy to read.
- Only the visible part is fetched and drawn, not the whole region every second.
- **AC:** From all 4 MB of main RAM, a few wheel notches reach single bytes inside a function, with no visible lag.

### CV-2 Hover
- A tooltip for the cell under the cursor shows:
  - the address range;
  - the containing function, data object or overlay from the knowledge store;
  - exec, read, write and DMA counts;
  - first and last frame seen;
  - code mode (ARM, Thumb, mixed);
  - flags: literal pool, card-loaded, decompressed, archived file (with its name), data-origin labels.
- The hovered cell is highlighted.

### CV-3 Click, double-click, right-click, drag
- **Click:** select the range. Inspector, Data origin and Write history follow the selection.
- **Double-click:** go to it in the Disassembly for exec coverage, or in Memory for read, write and DMA coverage.
- **Right-click:** Go to disassembly, Go to memory, Define function here, Define data here, Add bookmark, Cross-references, Write history, Track origin, Locate archived file.
- **Drag:** select a range, with the same right-click actions.

### CV-4 Legend and layers
- A legend shows the colour per access kind, and a brightness bar labelled with its minimum and maximum counts (the scale is logarithmic).
- A **Combined** mode colours each cell by its dominant access kind.
- Optional layers:
  - outlines of knowledge-store functions;
  - data objects;
  - overlay regions;
  - archived-file runs;
  - bookmarks;
  - the PC and cursor markers.
- An address ruler runs down the left edge.

## 7. ARM, Thumb and data in the Disassembly

### CD-1 Every row has a classification and a confidence

| State | Meaning | Evidence |
|---|---|---|
| ARM, confirmed | Executed as ARM | exec count > 0 and the ARM flag |
| Thumb, confirmed | Executed as Thumb | exec count > 0 and the Thumb flag |
| Mixed | Executed in both modes (rare: a bug or a trick) | both flags |
| ARM / Thumb, inferred | Inside a knowledge-store function, never executed | the function's mode |
| Data, confirmed | Read and never executed, or a literal pool | read or literal-pool flags, no exec |
| Data, defined | A data object in the knowledge store | `DataObjectAt` |
| Unknown | No evidence | none of the above |

### CD-2 How it looks
- A narrow **mode gutter** next to the heat column shows a badge: `A`, `T`, `D` or `?`. Confirmed is solid, inferred is outlined, unknown is a grey `?`.
- Unknown bytes still show as `.word`, in a muted style that differs from confirmed data.
- Confirmed and defined data show with their type when known: `.word`, `.hword`, `.byte`, `.asciz`, or a pointer with a link to its target. Defined data uses the knowledge-store type's field layout.
- The badge tooltip says why. For example: "Thumb, confirmed: executed 1,234 times as Thumb", or "ARM, inferred from function sub_02001234; never executed".
- A toolbar toggle **Decode only confirmed code** shows everything else as data or unknown.
- The panel menu has a legend.

### CD-3 Overrides
- Right-click → **Treat as ARM / Treat as Thumb / Treat as data / Clear override**, on a range. It is stored in the knowledge store with its author, is undoable, and exists as an API method for agents.
- When an override contradicts recorded evidence (marked data but executed), a ⚠ conflict marker appears. Conflicts are listed under a **Conflicts** filter in the Annotations panel.
- Decoding honours knowledge-store data objects.

### CD-4 The same answer everywhere
- The same classification appears in:
  - the Functions panel's Mode column (confirmed or inferred);
  - the Inspector;
  - the Memory panel's byte colouring;
  - coverage map tooltips;
  - the API: `disasm.get` and `metadata.get` return `classification` and `confidence`.
- **AC:** A never-executed function defined as Thumb shows "Thumb, inferred" in every view and the API. Once the game runs it, everything shows "Thumb, confirmed" without a manual refresh.

## 8. General discoverability

- **GD-1:** Every panel has a one-line description, shown on its tab, title bar and in the Panels palette.
- **GD-2:** The same gestures work in every table and tree: double-click goes to the item; right-click opens the shared actions (rename, comment, bookmark, cross-references, track origin).
- **GD-3:** A status-bar notice confirms every state change the user didn't make directly: recording started, a feature turned on implicitly, history wrapped, a workspace switched, a savestate's labels rebuilt.
- **GD-4:** Screenshot tests cover each new empty state and each built-in workspace, using the `DSWB_SHOW_PANEL`, `DSWB_GOTO` and `DSWB_LOCATE` hooks.

## 9. Build order

1. **ADS migration (WS-0)** with no behaviour change, then workspaces and the panel palette (WS-2, WS-3, WS-5).
2. **Prerequisites (PR-1 to PR-5):** the table, banners, auto-start, CLI flags, always-visible badges.
3. **Write history capacity (HC-1 to HC-3)**, then savestate handling (SS-1).
4. **Coverage map (CV-1 to CV-4)**, in that order.
5. **Code/data clarity (CD-1 to CD-4).**
6. **Embedded game screen (WS-1)**, after its spike. It can start in parallel with step 2, since it is the riskiest piece.
7. **Discoverability polish (GD-1 to GD-4) and Getting started (PR-6).**

## 10. Status

| Requirement | State |
|---|---|
| WS-0 ADS | Done. ADS 4.4 from MSYS2 (`mingw-w64-ucrt-x86_64-qt-advanced-docking-system`), linked dynamically |
| WS-2 Workspaces | Done. Each workspace also keeps the window geometry and the game-screen mode it was left in (built-ins leave both alone until first used; exports carry the mode but not the geometry). They are stored in the melonDS config as ADS layouts rather than as ADS perspectives in QSettings |
| WS-3 Finding panels | Done: grouped View menu, Panels palette, raise and focus. The tab menus are ADS's own (detach, pin to a side bar, close, close others) |
| WS-4 Ergonomics | Done through ADS: minimum panel size, drop overlays, auto-hide side bars |
| WS-5 Saving | Done: saved once a second when it changes; versioned; new panels go to their default place; off-screen floating panels come back |
| Tests | `ui_person_flow.py` switches workspaces with Ctrl+Alt+N, opens a panel from the palette, and checks what the window saved |
| PR-1..PR-5 | Built: `WBHost::Missing`/`Covers`, banners over panels, D-4 auto-start with a notice, arming in settings, `--write-history`/`--data-origin`, Reset game with tracking, clickable badges. `test_prereq.py` passes, including its keyboard-driven checks (Data origin is Ctrl+Shift+D: Ctrl+Shift+O never reached the window) |
| PR-6 | Built: a Getting started panel, shown until "Show at startup" is unticked, and Help > Feature requirements with live state. Seen in screenshots |
| SS-1 | Done as decided (D-3). Loading a state clears data-origin labels in memory and registers (coverage and calls stay), then labels what content proves: `arm9.bin`/`arm7.bin` and the best-matching overlay at their addresses, and copies of the modules (relocated code, autoload into ITCM), archived files and packed ROM files anywhere in main RAM and ITCM. A notice gives the count, `origin.get` has `state_loaded_frame`, and the Data origin panel explains unlabelled bytes. `test_recording.py` checks it |
| WS-1 | **Built, off by default** (View > *Game screen in the workbench*). The workbench hosts an extra melonDS window (`EmuInstance::createEmbeddedWindow`, with `MainWindow` reset to `Qt::Widget`) in the Game screen panel, with its own menus, screen options and input. TrackMania runs at 60 fps with the software and the OpenGL renderer, the separate window can be hidden, the panel can be floated and docked again (`DSWB_FLOAT_TEST`), and the app exits cleanly. The OpenGL crash had two causes: secondary windows shared their OpenGL context with their parent widget, assuming it was the main window; and the JIT's process-wide exception handler read `NDS::Current`, which is null outside the emulation thread, so the fault recursed into a stack overflow |
| CV-1 | Built: Ctrl+wheel zoom toward the cursor from a whole region down to one byte as an N×N cell, +, - and Fit, a synced cells spinbox, scrolling, middle-drag pan, the panel's full width, power-of-two rows, per-region zoom and position, only the visible part fetched. Left-drag selects (CV-3), so panning is the middle button or the scroll bar. Seen in screenshots on TrackMania |
| CV-2 | Built: hover highlight and a tooltip with the range, function, data object, module, counts, mode, classification, first frame seen, last write and flags. The first frame is kept per byte and per kind of access (exec, read, write, DMA: `RecordPage::First`, saved in sessions as `FirstFrames`), set when the byte's counter is still 0 so it costs no speed; memory grows by 16 bytes per recorded byte (+23 MB on the TrackMania race). It is this session's only, since frames don't line up across sessions. The Inspector shows it for the selected byte, and `metadata.get` returns it |
| CV-3 | Built: click selects (the Inspector, Data origin and Write history follow), drag selects a range, double-click opens the Disassembly (exec) or Memory, and right-click offers the actions in the list |
| CV-4 | Built: legend with log brightness bar and counts, Combined mode, layers (functions, data objects, loaded overlays, bookmarks, PC and cursor, ruler), and *Archived files (slower)*: main RAM equal to a ROM file or archived member, outlined and named in the tooltip, matched for the visible range |
| HC-1 | Done: filters for bulk runs, stack writes and unchanged writes (on by default), address ranges to keep and writers to skip, in the Recording panel and `recording.configure` |
| HC-2 | Done: a notice the first time history wraps, HIST shows the fill level and turns amber, the Recording panel and `writes.list` give the oldest frame kept |
| HC-3 | Done: a memory-mapped history file of any size; each saved session keeps its whole history as `<session>.dswbh` (moved when it ends, copied when saved without stopping), within a disk budget; `writes.list` `session`, `sessions.list` and the Write history panel's *History of* search one saved session |
| GD-1 | Done: each panel's one-line description is its tab tooltip, its View menu tooltip and status tip, and its line in the Panels palette (not on floating title bars) |
| GD-2 | Done: `WorkbenchWindow::AddressActions` builds the shared menu and `AddRowMenu` attaches it to Functions, Annotations, Bookmarks, Call stack, Call graph, Cross-references, Write history (the written bytes, plus the writer), Overlays, Search, Breakpoints and hits; Registers and the coverage map use it too. Double-click already went to the item. Seen in screenshots on TrackMania |
| GD-3 | Done: status-bar notices for recording started implicitly, a feature turned on for another, history wrapped, workspace switched and a savestate's labels rebuilt |
| GD-4 | Done: `ui_screens.py` takes 19 screenshots, every empty state and each built-in workspace |
| CD-1 | Done: `WBClassify` (unit-tested in `wbtests`) |
| CD-2 | Done: mode gutter with tooltips, muted unknown bytes, Decode only confirmed code, a legend. Defined data shows by its type: each field as `.byte`, `.hword`, `.word` or `.asciz` with its name, in the Disassembly and in `disasm.get`. Seen in screenshots |
| CD-3 | Done: `decode_override` table (migration 0005), Treat as… in the Disassembly, `kb.decode.set`, decode overrides / decode conflicts in the Annotations panel |
| CD-4 | Done for the Disassembly, Inspector, Functions Mode column, Memory panel (a line over each byte, solid when confirmed, dashed when inferred), coverage-map tooltips and the API. `test_recording.py` checks it |
| Data origin merges | Done (added after the limits review): each label naming several files records the instruction, CPU, frame and kind of merge that first made it and the two labels it joined; labels are keyed by their files and that merge site, so the history belongs to the value. The Data origin panel shows the tree (running totals collapsed to one step with a count), `origin.label` returns it, session files keep it (`LabelMerges`). On the race 96.4% of labelled main RAM names one file; every merged run has a history |

## 11. Still open

Nothing. Decided since: D-3 was confirmed and SS-1 built on it; ADS comes from the MSYS2 UCRT64
package; the game screen is an embedded melonDS window (WS-1), not a second screen panel or a
shared texture; write history is kept one session at a time, without search across sessions (HC-3).
