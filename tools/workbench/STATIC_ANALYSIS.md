# Static analysis

Debug → **Mark unreached code** follows code the recording has already reached and marks the
instructions and literal pools it has not. The marks use the same ARM, Thumb, and data
classifications, labeled **from analysis** (dashed amber in the disassembly gutter, the memory
view, and the coverage map’s **From analysis** layer). The walk identifies code the recording
has not executed. The marks are that result, taken from the bytes it read.

A finished run is a snapshot of addresses, mode, and reason, taken while the game was paused.
It stays across Resume. **Clear unreached marks** drops it. The next run replaces it from the
bytes as they are then, including edges the recording picked up in between. Marks are not
written to the knowledge store.

A run that has not finished when the game resumes is abandoned. That job reads memory on the
emulator thread, so once the CPU is running it can see a half-updated buffer. The notice
“Static analysis was discarded because the game resumed.” covers that case only, and any
earlier finished snapshot stays.

While a run is going, the status bar says which CPU is being walked and how many instructions
have been marked, updated as each batch of 2048 is committed. The same menu item becomes
**Cancel static analysis** and stays enabled. Cancel stops the walk and does not replace the
previous snapshot. The marks appear together when the run finishes.

The summary gives instruction and literal counts per CPU, how many places were left unmarked,
the first of those, and whether the walk stopped early. It stays in the status bar until the
next run, a clear, or a new game. The first unmarked address opens the disassembly there.
When the run finishes, if the disassembly was sitting on unknown bytes and not already inside
a mark, it moves to the first marked instruction on the current CPU.

**Decode only confirmed code** shows the marks as data. The coverage map still draws them.
When that option is on, the summary says the disassembly is set to confirmed code only, and
that the coverage map still shows the marks.

**File → Import static analysis (JSON)…** loads a knowledge-store file. The Debug commands are
**Mark unreached code** and **Clear unreached marks**. The tooltip says the marks stay until
they are cleared or the command is run again, and that they are not written to the knowledge
store. When the item is disabled, the tooltip says “Pause the game first (F5).” or
“No game is running.”

## Later: bytes that were overwritten

An overlay swapped out, or bytes decompressed over a block the walk marked, will still say
“from analysis” while the disassembly decodes whatever is there now. A new run already
rebuilds the snapshot from current memory. A later pass can drop a range whose bytes no
longer decode as the instruction that was marked, or whose overlay is no longer loaded.
That is a refresh of stale ranges, not a reason to clear the map on Resume.
