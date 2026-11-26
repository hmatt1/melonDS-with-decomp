# Workbench gaps checklist

Open gaps in components 1–3 that matter for the TrackMania DS decomp. Each item says what is
wrong and what will show it is fixed. Items are checked off as they land.

## Worth fixing

- [x] **1. Part of the physics struct has no origin.** 16 of the 91 words that
  `load_vehicle_physics_parameters` (`020a87f4`) writes carry no `vehicles.luac` label.
  *Done when:* each unlabelled word is explained: either it doesn't come from the file
  (constant, computed default, pointer), or a propagation bug is fixed. The `--struct` check then
  asserts on the words that should be labelled.
  *Result:* the gear ratios went through the ARM9 hardware divider, and
  labels didn't pass through its IO registers. Data origin now models the divider and the square
  root unit. 81 of 91 words are labelled (89%). The other 10 are counters, flags, zero defaults,
  function pointers, and the gear count (a Lua table length, which is control flow rather than
  data). The test asserts this.
- [x] **2. Agents can't do everything a person can.** The API has no knowledge-store deletes
  (function, data object, type, label, bookmark). It also can't run Compare, import static
  analysis, set a function's source file, or record build results.
  *Done when:* each of these is a `kb.*` method that goes through the same code as the window,
  attributed and undoable where the window's version is, and covered by a test.
  *Result:* added `kb.function.update`, `kb.function.delete`, `kb.data.delete`, `kb.label.delete`,
  `kb.type.delete`, `kb.bookmark.delete`, `kb.build.compare`, `kb.build.record` and
  `kb.import.static_analysis`. Compare and import now share one implementation with the window.
  Fixing this also fixed a bug the window had: deleting a function failed when it had
  annotations, and undo lost its build results. Deletes now take their dependent rows along as
  one change that a single undo reverses.
- [x] **3. Write history overflows during loading.** Unpacking writes millions of bytes, so the
  4M-entry history keeps only the last seconds of a level load.
  *Done when:* history can skip writes that aren't wanted: writes by chosen PC ranges, writes
  outside chosen address ranges, and the unpacker's output automatically. A TrackMania load to
  the race then keeps the vehicle parameter loader's writes.
  *Result:*
  * New filters, configurable in the Recording panel and the API:
    * skip bulk runs, stack writes and unchanged writes (all three on by default);
    * address ranges to keep, and writers to skip.
  * A memory-mapped history file of any size, plus a notice and status when history wraps.
  * From boot to the race, TrackMania writes about 74 million times to main RAM. The filters keep
    8.6 million of those, so a 512 MB file holds them all, including the loader's 92 writes.
    `--struct` checks this.
- [x] **4. Large API queries are slow or time out.** For example, `writes.list` over the whole
  history.
  *Done when:* long results page (`skip` / `limit` with a `total`), and a full-history query
  returns in well under the client timeout.
  *Result:* `writes.list` pages newest-first (`skip`, `limit` up to 100,000, `total`, `truncated`)
  in two passes, so memory use doesn't grow with history. `metadata.get` and `coverage.get` report
  where to continue (`next_addr`). On TrackMania's 8.6M-entry history, the loader's writes come back
  in under 0.1 s and a 100,000-entry page in 0.7 s.

## Smaller or edge cases

- [x] **5. Tail calls are only recognised into functions already seen called with BL.** Functions
  reached only by a tail call, or by a conditional branch, are credited to the caller.
  *Done when:* functions known to the knowledge store also count as entries, and conditional
  immediate branches to a known entry count as tail calls. The test ROM covers both.
  *Result:* the recorder gets the knowledge store's function entries whenever they change, and
  ARM and Thumb conditional branches count. The test ROM has a function reached only through a
  tail call, which is recognised once the knowledge store defines it, and a taken conditional
  tail call.
- [x] **6. *Follow table lookups* judges pointers by value alone.** Data that looks like a RAM, IO
  or VRAM address loses its label in that mode.
  *Done when:* a value counts as a pointer only if it is also used as one, and the test ROM
  covers a data value in the address range that keeps its label.
  *Result:*
  * Each register now has two labels: its ordinary label, and an address label (what a load
    through it takes).
  * Pointer arithmetic drops the pointer's part only from the address label. A register's ordinary
    label is narrowed only once it has actually been used as an address.
  * Data in the address range keeps its label (test ROM: file word `0x02345678` plus a counter).
  * The physics struct with `--struct --follow` is now 89% labelled with 0% noise, down from
    14% noise, because stored pointers no longer carry their history into the data they point at.
- [x] **7. Content labelling covers files inside `vfs.zip` only.** Plain NitroFS files that game
  code unpacks are not covered.
  *Done when:* plain NitroFS files and their LZ-decompressed forms are part of the content index,
  so code that copies or unpacks them gets exact labels. Covered by a test.
  *Result:*
  * Plain NitroFS files in a BIOS compression format are now indexed by their unpacked form: LZ77
    types 0x10 and 0x11 (optionally behind an `LZ77` tag), Huffman and run length.
  * Once such a file is read, unpacker output equal to it is labelled with the file.
  * The test ROM unpacks `plain.lz` in software through a lookup table. Its output is unlabelled
    without this change and `/plain.lz` with it.
  * Files stored as they are aren't indexed: a copy keeps its label anyway. Indexing them cost
    TrackMania about 10% loading speed (its sound archives are read throughout), for no new labels.
  * TrackMania has no compressed plain files, so its loading speed is unchanged.
  * Also fixed: `workbench/Archive.h` shadowed libarchive's `archive.h` on Windows, so a fresh
    build failed. It is now `GameArchive.h`.
- [x] **8. Loading is slow with data origin on every ROM file** (56–58 fps uncapped for the
  race-loading steps, measured again at the commits since).
  *Done when:* content matching probes only at content-defined anchors instead of every byte
  offset, and the TrackMania race-loading steps are clearly faster than now, measured the same
  way.
  *Result:*
  * Measured first: content labelling took loading from 66 to 56 fps. Write-run tracking cost 3 fps
    of that, and the index search 7.
  * The search still probes every byte offset, but now with a rolling hash. A one-bit-per-bucket
    filter (about 16 buckets per key) rejects almost every window before the sorted keys are
    searched.
  * This finds exactly the runs the old search did, so I dropped the sparse anchors: those would
    miss some short runs.
  * Loading steps went from 56 to 62 fps, and the menus from 95 to 103. With no search at all it
    would be 63. Labels are unchanged: `--struct` still gives 89% with 0% noise.
- [x] **9. Compressed overlays are only detected through card reads.**
  *Done when:* overlay detection compares against the unpacked image of compressed overlays (the
  backward LZ format). Covered by a unit test.
  *Result:*
  * Overlay detection now compares running code with a compressed overlay's unpacked image. The
    image is unpacked the first time it's needed and kept for that ROM.
  * The unit test covers a hand-built stream, a vector from `ndspy`'s backward LZ
    compressor, and malformed footers.
  * Separately, five larger `ndspy`-compressed samples (9–20 KB) all unpacked byte for byte.
  * TrackMania's overlays and `arm9.bin` aren't compressed, so this has no TrackMania test.
- [x] **10. The latest UI changes are unchecked:** the Call graph's tail-call marker, Track this
  file, `load (ran)` overlay events, and the Archive panel's cramped file table.
  *Done when:* each is seen in a screenshot and the Archive table shows its columns at the
  default size.
  *Result:* each was checked in a screenshot:
  * the Call graph lists the test ROM's two tail callers as `(tail call)`;
  * the Archive panel shows **Track this file** next to **Locate in RAM**, and Path, Size and
    Packed are readable at the default size (further columns scroll);
  * the Overlays panel shows `load (ran)` for overlay 2 after a savestate.

  The Overlays table's last header ("Compressed") was cut off at the default size. Fixed later:
  shorter headers (Start, End, Packed, Loaded, with tooltips), and every table's columns start as
  wide as their headers instead of 100 px each.
- [x] **11. ARM7 is untested** for overlay detection, tail calls and content labelling.
  *Done when:* the test ROM runs its own ARM7 code with a tail call and an archived-file read, and
  `test_recording.py` checks them.
  *Result:*
  * The test ROM now has its own ARM7 core (`testrom/arm7/`, calico's default core plus test code).
    The ARM9 passes it a shared struct over PXI.
  * The ARM7 copies `Models/test.nsbmd`, which the ARM9 read out of `vfs.zip`, twice: as it is,
    and through a lookup table. Both copies carry the file's label. The second fails without
    content labelling.
  * This found a bug in call tracking. ARMv4T interworking veneers (`bx pc`, then `b target`) were
    only followed one step, so every Thumb-to-ARM call on the ARM7 was credited to the veneer, and
    the tail call went unseen. A callee whose first instruction is an unconditional branch now
    forwards the call too.
  * Overlay detection on the ARM7 is still untested: making an ARM7 overlay with devkitPro's tools
    isn't practical, and TrackMania has none.
- [x] **12. The `--struct` test is hard-wired to one loader address.**
  *Done when:* the test finds the loader itself. A new query returns the ranges labelled with a
  source, and the function that wrote most of those words outside the file's own RAM copy is the
  loader.
  *Result:*
  * The new query is `origin.ranges`. The planned rule ("wrote the most labelled words") did not
    hold: Lua internals and per-frame physics code write far more words derived from
    `vehicles.luac` than the loader does.
  * The rule that works is the densest block of labelled data words that was written once while
    loading. Pointers, copies of archived files and words rewritten every frame are left out.
  * That picks `020a9074` (64 words of the struct at `0223b9f0`, written in frame ~1490). The next
    candidates have 42 and 39. The search takes about 1 s.
  * The old hard-wired range (`020a87f4` + 0xB00) covered several functions. `020a87f4` itself is
    a small wrapper.
  * Counted for `020a9074` alone: 66 of 71 words are labelled (93%). The other 5 are zero defaults.
  * Those zeros are also absent from write history: history leaves out writes that don't change
    memory. The history check now allows for that.

## API parity with the window

Everything the window shows should be readable through the API, and everything an agent
might want to point the person at should be controllable. Settings (commands melonDS runs,
memory writes, the knowledge-store path), the person's own undo, workspaces and layout stay
out on purpose: a connected client must not be able to raise its own access.

- [x] **13. The API can't read the knowledge store.** Only `kb.changes.list` existed. An agent
  could open `knowledge.db` itself, but the API couldn't see the Functions, Annotations,
  Bookmarks and Types lists, labels, data objects, decode overrides, a function's source file
  and Compare attempts, or imported decompiler text.
  *Done when:* read methods return what those panels show, with the panels' filters, and a
  test checks each list.
  *Result:* `kb.list` (one `kind` per call, filtered and paged) and `kb.function.get`. They read
  the same caches as the panels; the Calls column and decode conflicts come from the same
  emulation-thread code the panels use (`WBQ::ExecCount`, `WBQ::DecodeConflict`), and the
  Decompiler panel's renaming moved into `KnowledgeStore::NameDecompiled` for both.
  Source text is only returned from inside the source root. Writing the test showed that a data
  object typed `u32[4]` without a size had size 0 in the store, so neither the window nor the
  API found it from an address inside it; basic types, pointers and arrays now give a size.
- [x] **14. Panel tools have no API.** The Cross-references panel (imported references and
  observed reads, writes and calls), the Overlays panel (table, load events, which is loaded),
  and the Search panel (bytes, ASCII and Shift-JIS strings, value scans across pauses).
  `disasm.get` can't view another overlay epoch.
  *Done when:* `xrefs.get`, `overlays.list`, an overlay option on `disasm.get`, and
  `search.bytes` / `search.string` / `search.scan` return what the panels show, with tests.
  *Result:* the methods share their code with the panels (`WBQ::RuntimeXrefs`,
  `WBQ::OverlayImage`, `WBQ::FindPattern`, `WBQ::ScanFirst` / `ScanNext`). Value scans are kept
  per connection, the newest 8. On TrackMania, the epoch view of overlay 3 (not loaded; overlay 2
  is) shows its own bytes and the 9,901 instructions it ran in the menus, the same as the panel;
  searches and scans of main RAM take 10–20 ms. Found and fixed on the way: compressed overlays
  showed `??` in the epoch view (they are now unpacked), text with characters outside Latin-1
  searched for `?` instead, a scan value outside the type's range (a negative u32) silently
  matched nothing, and the Cross-references panel cut its addresses to `0200...`.
- [x] **15. Breakpoints can only be set and cleared.** Toggling one loses its hit count.
  *Done when:* `breakpoint.update` enables, disables and changes the condition, keeping the
  hit count.
  *Result:* `breakpoint.update`, and *Edit condition* in the Breakpoints panel. A bug bash showed
  that while paused, breakpoints set or changed through the API never reached the window, and
  a panel brought to the front kept what it last showed until the game ran. Breakpoint changes
  now refresh the Breakpoints panel and the disassembly's markers, and a panel refreshes when it
  is brought to the front.
- [x] **16. An agent can't point the window at anything.** No way to go to an address, select a
  range, open a panel, switch CPU or screenshot the workbench window.
  *Done when:* `ui.show` (address, selection, panel, CPU) and `ui.capture` on the control
  endpoint, so an agent can say "look here" and the person sees it.
  *Result:* both are control methods, so a read-only client cannot pop the window or receive a
  picture of it. `ui.show` moves the cursor (follow-PC turns off, and Back returns to where the
  person was), highlights a range, opens a panel and brings the window forward. A selection on its
  own leaves the cursor, as the window's own Write history action does. `note` is a status-bar
  message. `ui.capture` is a PNG of the window or of one panel, taken after that view has fetched,
  and it does not raise the window. A bad call does not open the window, and a closed panel is an
  error. `ui_point.py` checks this on the test ROM, including a screenshot: the disassembly at
  `02000000` with 16 bytes selected, Write history of `02000100`, and the note in the status bar.
- [ ] **17. NARC support is checked only on the test ROM.** The container reader was built from the
  format and checked on hand-built NARCs (with and without names, packed whole, nested three
  levels) and on TrackMania's two SDATs, but on no real game that keeps its data in NARCs.
  *Done when:* a NARC-heavy game's containers list with names, card reads of a `.narc` and the
  unpacked files of a `.carc` are labelled as the files inside, and `container.locate` finds a
  loaded graphics file in RAM.

## dsd import and vtables

From `docs/2026-11-20_workbench_vtable_recommendation.md` in the TrackMania DS decomp repository.

- [x] **18. The knowledge store knows almost none of what the decomp project knows.** It had 2
  named functions; `dsd-config` has 1,756, with every function's size and mode and 41,065
  references. The importer also reloaded the whole store after every row, rewrote every row on
  a second import, and put every reference into the first overlay at its address.
  *Done when:* after an import every function in `symbols.txt` is in the store with dsd's name,
  mode and size; a second import changes nothing; a reference from overlay 3 lands on `ov9_0003`.
  *Result:* `python -m dswb.dsd_import` (README, "dsd configuration and vtables"). The import is
  one transaction and one reload, leaves an unchanged entry alone, takes `from_overlay` and
  `to_overlay` on references, and reports entries it could not import. On TrackMania DS: 7,393
  functions, 183 labels and 41,065 references in 0.9 s; the second import took 0.3 s and added
  no change-log rows. dsd owns names, sizes and modes, so an import overwrites a name given by
  hand.
- [x] **19. Interrupts and BIOS calls were recorded as ordinary calls.** Since 2026-10-13 the
  veneer rule followed an immediate branch, and the BIOS exception vectors are branches, so each
  interrupt added a call edge from the interrupted instruction to the BIOS handler.
  *Done when:* a session has no call-kind edge into the BIOS.
  *Result:* the forwarded edge keeps the frame's kind, and an interrupt's edge has no call site.
  `test_recording.py` checks the test ROM's interrupts and `swi` calls. Sessions saved between
  2026-10-13 and this fix keep the wrong edges.
- [x] **20. Nothing finds the game's C++ classes.** Vtables can be found from dsd's references,
  and saved sessions say which were used.
  *Done when:* a tool lists every table with its slots, constructors, call sites and storing code.
  *Result:* `python -m dswb.vtables`. On TrackMania DS's 19 sessions, main module: 390 candidate
  tables, 315 indirect call sites, 267 with a target in a table. Decoding the slot from the
  instructions before the call ties 257 of them, reader addresses tie 203, and 265 are tied one
  way or the other. 39 tables were stored into objects by 71 code addresses. Recording the slot
  address in the recorder (the recommendation's item 4) is therefore not built.
- [x] **21. Call edges did not say which overlay they ran in.** Overlays 2 and 3 of TrackMania DS
  load at one address, so their edges shared keys.
  *Done when:* two overlays at one address keep their edges apart, and every edge in the shared
  range names an overlay.
  *Result:* `CallEdge` has the overlay of the call site and of the callee (session schema 2;
  schema 1 files still load, with none). `calls.get` takes `overlay` and returns both on each
  edge; the Call graph and Cross-references panels use them. On TrackMania DS every edge from
  and into overlay code names its overlay (`agent_scenario.py`). The first call into an overlay
  after a savestate was recorded before the overlay was recognised; the recorder now settles
  the callee's overlay first. Sessions saved before this carry no overlay on their edges.
- [x] **22. Runtime evidence is filed per function without its overlay.** `CollectSession` picked
  the function by address, so evidence for code two overlays share could land on the other
  overlay's function, and an overlay function's execution counts came from whichever overlay was
  loaded when the session was saved.
  *Done when:* evidence for a function in overlay 3 is stored on the overlay 3 function.
  *Result:* the function snapshot knows each function's overlay; counts come from that overlay's
  own records; calls are filed by the overlay their edge names, and data accesses by the overlay
  loaded at the reader or writer. A TrackMania race session (overlays 2 and 3 both ran): at
  `021159f8`, where both have a function, overlay 2's has 11 entries and 4,209 instructions and
  overlay 3's 9 and 462; all 2,248 call entries are on the overlay that ran them.
- [x] **23. Cross-references and Call graph could stay empty after `ui.show`.** Both follow the
  cursor only while shown, and `ui.show` moves the cursor before opening the panel. `ui.capture`
  did not wait for their queries, which take seconds on a long TrackMania session. And on the
  window's first show, the restored layout brought Getting started forward over the panel asked for.
  *Done when:* `ui.show` with an address and one of these panels, then `ui.capture` at once,
  shows that address's rows.
  *Result:* both panels catch up when they come forward and are busy while their queries run;
  `ui.show` brings its panel forward again after the first layout; a capture brings its panel's
  tab forward again if something buried it. `ui_point.check_followers` (run by
  `test_recording.py`) fails on the old code; checked on TrackMania by hand.
- [x] **24. A dsd import left wrong functions and lost names.** Functions the workbench had
  detected on its own stayed on top of dsd's (31 in TrackMania DS, most starting 2 or 4 bytes
  after the real entry); a dsd placeholder name cleared a name given in the workbench; and
  unnamed functions in overlays 2 and 3 had the same name, `func_020ffe00`.
  *Done when:* after an import no detected function overlaps a dsd one, a name only the
  workbench has survives, and an unnamed overlay function is named with its overlay.
  *Result:* the import file's `covers` ranges make its list the whole truth there (detected
  functions it lacks are deleted, a person's or agent's are kept); an entry with no `name` keeps
  the store's name; `dsd_import --apply` reviews the store first; unnamed functions are
  `func_ov003_020ffe00` everywhere a name is shown, and that name is understood when typed or in
  decompiler text. On a copy of the TrackMania DS store: 31 removed, the review listed both of
  `matcher-agent-1`'s names dsd replaces and its one conflicting function, the 140 ARM7 functions
  were untouched, and a second import changed nothing.
- [x] **25. Names given in the workbench do not reach dsd.** `dsd_import --apply` listed them, but
  adding them to `symbols.txt` was by hand.
  *Done when:* a command writes them into `symbols.txt` as a change to review in git.
  *Result:* `python -m dswb.dsd_sync` (README, "dsd configuration and vtables"): writes them over
  dsd's default names, records them in `workbench_names.tsv` so a dsd-config made from scratch gets
  them back, checks the build (and puts everything back when it fails), then imports. It runs when
  a person asks, not on every rename. TrackMania DS, on a copy of the project: three names written,
  the ROM still matches, two refused, a second run writes nothing.
- [x] **26. A re-import left behind what dsd no longer lists.** A function moved, merged or deleted
  in `symbols.txt`, and its labels and references, stayed in the store.
  *Done when:* after such a change a re-import leaves the store as dsd has it.
  *Result:* an import with covered ranges writes as its tool (`dsd`) and deletes, in its ranges,
  the functions and labels its earlier import made that it no longer lists, and the imported
  references; a person's or agent's rows are kept. On TrackMania DS, merging `luaV_execute`'s
  four pieces in a copy of `symbols.txt`: the re-import removed the three, and a repeat changed
  nothing.
- [x] **27. Disagreements with dsd were hidden.** The import deleted the workbench's conflicting
  functions with only a count, and the recorder could not tell a call from a `bl` used as a
  jump, so calls into the middle of a function were noise.
  *Done when:* the recordings' evidence against dsd's function list is reported, with jumps told
  from calls.
  *Result:* each call edge records whether the call came back (only a return instruction counts:
  a jump table landing after a `bl` does not); a branch back into code the function already ran,
  or to an entry only ever reached by `bl` jumps, is not a tail call; `dswb.dsd_check` sorts what
  the recordings saw; the import's review lists each function it removes. On TrackMania DS the
  check found that dsd splits `luaV_execute` into four pieces (README, "dsd configuration and
  vtables"). Checked against the code: `inflate`'s and `func_02049d30`'s `bl` jumps are a loop
  `continue` and an early `return`.
