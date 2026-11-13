<p align="center"><img src="https://raw.githubusercontent.com/melonDS-emu/melonDS/master/res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS</b></h2>
<p align="center">
<a href="http://melonds.kuribo64.net/" alt="melonDS website"><img src="https://img.shields.io/badge/website-melonds.kuribo64.net-%2331352e.svg"></a>
<a href="http://melonds.kuribo64.net/downloads.php" alt="Release: 1.1"><img src="https://img.shields.io/badge/release-1.1-%235c913b.svg"></a>
<a href="https://www.gnu.org/licenses/gpl-3.0" alt="License: GPLv3"><img src="https://img.shields.io/badge/License-GPL%20v3-%23ff554d.svg"></a>
<a href="https://kiwiirc.com/client/irc.badnik.net/?nick=IRC-Source_?#melonds" alt="IRC channel: #melonds"><img src="https://img.shields.io/badge/IRC%20chat-%23melonds-%23dd2e44.svg"></a>
<a href="https://discord.gg/pAMAtExcqV" alt="Discord"><img src="https://img.shields.io/badge/Discord-Kuribo64-7289da?logo=discord&logoColor=white"></a>
<br>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml/badge.svg" /></a>
</p>
DS emulator, sorta

The goal is to do things right and fast, akin to blargSNES (but hopefully better). But also to, you know, have a fun challenge :)
<hr>

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

## How to build
See [BUILD.md](./BUILD.md) for build instructions. This fork needs a few more
packages than upstream melonDS (SQLite, zlib,
Python 3); they are listed there.

## Decomp workbench

This fork adds a second window for decompiling DS games: disassembly,
write history ("who wrote this field, from where, how wide"), watchpoints,
value search, data origin, cross-references, call graph, coverage, naming,
a compare step for your decompiled C, and a JSON-RPC API with a Python client.

Before you start:

* **Turn the JIT off** (Config > Emu settings). Recording and breakpoints
  need the interpreter; with the JIT on, recording stays off.
* Workbench data goes under `~/.config/melonDS/workbench/` (Linux), one
  folder per game, `<GAMECODE>_<sha1 prefix>/`: knowledge store, saved
  sessions, compare runs. `--kb` points it at a file of your own instead.
* From a running melonDS: **System > Decomp workbench...** (Ctrl+Shift+W).
* Debugger keys (Debug menu): Continue Ctrl+F5, Break Ctrl+Shift+F5, Step into F11,
  Step over F10, Run to cursor Ctrl+F10, Run one frame Ctrl+F11, Toggle breakpoint
  Ctrl+F9. Recording: Ctrl+R, write history Ctrl+Shift+H, data origin Ctrl+Shift+D.
  Right-click menus name the same commands with the same keys.

### Command line options

| Option | Effect |
|---|---|
| `-w`, `--workbench` | open the workbench window on startup |
| `--record` | turn recording on at boot (history and origin stay off unless asked) |
| `--write-history` | recording + write history at boot |
| `--data-origin` | recording + data origin at boot |
| `--no-record`, `--no-write-history`, `--no-data-origin` | leave that off |
| `--kb <path>` | knowledge store (SQLite) to use |
| `--api` | start the control/query API on `127.0.0.1` |
| `--api-port <port>` | read port (control is port+1) |
| `--api-memory-write` | allow `memory.write` through the API |
| `-b auto/always/never` | boot firmware first (needs BIOS/firmware dumps) |

`melonDS --help` prints the full list.

### Launch commands for common scenarios

The examples use `build/melonDS` and a ROM at `~/ds-decomp-bootstrap/rom/atc.nds`;
substitute your own.

**Explore a game: open the workbench, everything on.**

```sh
build/melonDS -w ~/ds-decomp-bootstrap/rom/atc.nds
```

**Find out who writes the fields of an unk struct** (write history plus
watchpoints). Recording and write history are what you need; leave data origin
off to save memory and time:

```sh
build/melonDS -w --write-history --no-data-origin game.nds
```

Then select the struct in Memory, open the Write history tab, and set a write
watchpoint on the field (Debug > Continue, Ctrl+F5, runs to the next write).

**Find where data in RAM came from** (ROM file, offset, decompression):

```sh
build/melonDS -w --data-origin game.nds
```

Data that arrived with a loaded save state has no origin; boot the game and
play to the spot instead of loading a state.

**Search for a value** (e.g. a counter going from 999 to 1000), with no
recording overhead:

```sh
build/melonDS -w --no-record game.nds
```

Value search and breakpoints work without recording; history, origin,
cross-references and coverage don't.

**Work in your decomp project's own knowledge store** (names, types and
comments travel with the project):

```sh
build/melonDS -w --kb ~/ds-decomp-bootstrap/workbench/atc.sqlite game.nds
```

Set the project folder in **Build > Compare setup** ("Decomp source tree") to
compare your compiled functions against the game's.

**Drive it from a script or an agent** (Python client in
`tools/workbench/python`):

```sh
build/melonDS -w --api game.nds &
export PYTHONPATH=$PWD/tools/workbench/python
export DSWB_API_INFO=~/.config/melonDS/workbench/api.json
python3 -m dswb status.get
python3 -m dswb execution.control '{"action": "pause"}'
python3 -m dswb memory.read '{"addr": "0x02078d30", "size": 16, "decode": "u32"}'
```

On Linux the client doesn't find `api.json` by itself: set `DSWB_API_INFO`
or pass `info_file=` to `Client.connect()`.

**Let a script write memory too** (patching values while testing):

```sh
build/melonDS -w --api --api-memory-write game.nds
```

**Boot through the firmware** (games that read user settings, or to watch
the boot itself; needs BIOS and firmware dumps set in Emu settings):

```sh
build/melonDS -w -b always game.nds
```

**Game inside an archive:**

```sh
build/melonDS -w games.zip -a "atc.nds"
```

**Inspect a saved recording session** without starting melonDS:

```sh
PYTHONPATH=tools/workbench/python python3 -m dswb --session \
    ~/.config/melonDS/workbench/<GAME>_<sha>/sessions/<file>.dswbs
```

More: `tools/workbench/README.md` (API reference).

## Screen filters

View > **Screen filter** picks how the DS screens are drawn: **Off**, **LCD** or **CRT**. One
filter is on at a time, and the choice applies to every window of an instance. **Turn filter
on/off** (Alt+F) switches between Off and the filter used last.

Each filter has its own settings window, also under View > Screen filter. At the top of each,
**Use the LCD/CRT filter** turns that filter on (unticking it turns the screen filter off); the
settings can be changed whether or not it is on. Every slider's tooltip says what it does and its
default. The preset row saves and applies named sets of settings (`lcd-presets.json` and
`crt-presets.json` next to `melonDS.toml`; presets saved under the old pixel grid name are
carried over). Picking **Default** always restores the built-in values, even when it is the only
entry.

### LCD filter

Draws the DS screens the way a real DS screen looks: thin lines between pixels, the coloured LED
stripes of each pixel (DS Lite, DSi or DSi XL layout) and an optional blur. Its settings set how
dark the lines are, the stripe layout and strength, and the blur.

### CRT filter

Draws the screens like an old CRT television, after the shader in spritecook.ai's "CRT shader for
pixel art": a sideways-only signal blur in YIQ (sharp brightness, softer colour, so 1-pixel
dithering melts into an even shade), Gaussian scanlines that widen on bright pixels, an
aperture-grille mask of red, green and blue stripes in screen pixels, glow, edge darkening and
screen curvature. Its settings: curvature, picture blur, colour bleed, the scanline width on dark
and on bright pixels, mask strength and width, glow, edge darkening and brightness.

It is made for whole-number zooms (View > Screen sizing > Force integer scaling); uneven zooms
give the scanlines a wavy pattern. The defaults are tuned for 3x, the usual zoom: a 1 px mask puts
one red-green-blue triad on each DS pixel, and Brightness 1.25 with Glow 0.15 keeps mid-tones about
as bright as without the filter instead of washing out the bright screens DS games tend to have
(the article's values, Brightness 1.5 and Glow 0.26, suit darker pixel art). At 6x or more, set
the mask width to 2. The edge of the curved screen is antialiased.

### What draws them

What draws a filter follows Config > **Video settings**:

* an OpenGL display, or the OpenGL (Classic) or OpenGL (Compute shader) renderer: the OpenGL
  screen shaders. The CRT one does the work that depends only on DS pixels (the colour signal and
  the glow) once per frame at 256x192, so the per-pixel pass reads 19 texels instead of 43;
* the Software renderer without the OpenGL display: software versions of the same filters, on
  the CPU. They are the fallback, so the filters work without OpenGL; they look the same at
  whole-number zooms.

The software filters use half the logical cores, 2 to 4 threads, by default. To set the number,
put `PixelGridThreads` under `[Instance0]` in `melonDS.toml` (0 means automatic). Per frame, for
both screens:

| Filter | Zoom | 2 threads | 4 threads |
|---|---|---|---|
| LCD (DSi XL stripes and blur) | 4x | 2.6 ms | 1.4 ms |
| CRT, flat (curvature 0) | 3x | 3.7 ms | 2.1 ms |
| CRT, curved (the default) | 3x | 8.6 ms | 4.6 ms |
| CRT, flat (curvature 0) | 4x | 6.0 ms | 3.2 ms |
| CRT, curved (the default) | 4x | 14.1 ms | 7.4 ms |

A screen the layout doesn't show (Top only, Bottom only) isn't filtered.

## Credits

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS, reporting issues, suggesting shit, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
