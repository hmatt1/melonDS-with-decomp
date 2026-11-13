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
dark the lines are, the stripe layout and strength, and the blur. The grid lines are balanced
against the stripes for each pixel's own light, so they never tint the picture, however dark they
are.

Built-in presets: **DS (original)**, **DS Lite**, **DSi**, **DSi XL**, **Grid only (no stripes)**
and **Soft and glowing**. What they rest on: the DS and DS Lite share a 0.24 mm dot pitch; the DSi
XL's pixels are about 1.4 times larger, and reviews describe its gaps between pixels and its RGB
stripes as plainly visible, hence the darker grid and stronger stripes. No close-up photo
confirms any model's stripe order, so the presets use the Subpixels setting's layouts (the
original DS takes the DS Lite's). The filter has no colour controls, so the original DS's dimmer,
washed-out panel isn't part of its preset.

### CRT filter

Draws the screens like an old CRT television, after the shader in spritecook.ai's "CRT shader for
pixel art": a sideways-only signal blur in YIQ (sharp brightness, softer colour, so 1-pixel
dithering melts into an even shade), Gaussian scanlines that widen on bright pixels, an
aperture-grille mask of red, green and blue stripes in screen pixels, glow, edge darkening and
screen curvature. Its settings: curvature, picture blur, colour bleed, the scanline width on dark
and on bright pixels, mask strength and width, glow, edge darkening and brightness.

Each scanline is a beam: a Gaussian across the rows whose width follows its brightness. Every DS
row whose beam reaches a pixel lights it, so a bright line's glow spreads evenly up and down;
beams fade smoothly to nothing past 3 of their widths. A pixel shows the light that falls on it,
the beam averaged over the pixel's height, not the beam's value at its centre point. Beams
point-sampled 2 or 3 pixels apart ripple against the screen's pixel grid where the screen is
curved or the zoom uneven; averaged, they keep the same brightness at any position.

It is made for whole-number zooms (View > Screen sizing > Force integer scaling): there every
scanline has the same shape. At uneven zooms they look slightly sharper or softer from row to
row, but brightness stays even. The defaults are tuned for 3x, the usual zoom: a 1 px mask puts
one red-green-blue triad on each DS pixel, and Brightness 1.25 with Glow 0.15 keeps mid-tones about
as bright as without the filter instead of washing out the bright screens DS games tend to have
(the article's values, Brightness 1.5 and Glow 0.26, suit darker pixel art). At 6x or more, set
the mask width to 2. The edge of the curved screen is antialiased.

#### Video signal

The CRT settings' **Video signal** section sends the DS picture to the TV the way a real
console's video encoder would, NTSC or PAL. The signal is simulated at the console's master clock
and decoded the way a consumer TV decodes it. Colour bleed, rainbows on fine detail and dithering that
blends into solid colour all come out of that simulation, not from effect sliders. It is off by
default, which gives the simple blur set by Picture blur and Colour bleed.

* **Console:** whose encoder and timing.
  * **NES / Famicom:** the NES has no encoder: its picture chip makes the composite wave itself,
    a square wave per palette entry. So the DS picture is first shown in the NES's colours (each
    colour as its nearest palette entry), then sent exactly as the NES sends them. Its strong
    diagonal artifacts come from that square wave and its 1.5 pixels per colour cycle. Composite
    and RF only.
  * **Mega Drive 1 / Genesis (H40 or H32):** the same timing as the Mega Drive 2, with the model
    1's encoder and the filter networks Sega put around it, which soften brightness and colour
    more. No S-Video.
  * **Mega Drive, 320 wide (H40):** 1.875 DS pixels per colour cycle. That is the frequency of
    1-pixel dithering, so dithering blends and turns into colour. Every line is exactly 228
    carrier cycles, so the artifacts stand still.
  * **Mega Drive, 256 wide (H32):** 1.5 DS pixels per colour cycle, also still.
  * **SNES:** 1.5 DS pixels per colour cycle. The carrier moves a third of a cycle per line, so
    the artifacts run diagonally. Every other frame has a short line, so the pattern alternates
    between two frames.
  * **Mega Drive PAL (H40 or H32):** the European signal, with a 4.43 MHz carrier and V's phase
    flipped every line. 1.5 (H40) or 1.2 (H32) DS pixels per colour cycle, so dithering doesn't
    sit on the carrier the way it does on a 320-wide NTSC Mega Drive. Lines are exactly 285
    cycles, so the artifacts stand still, but the odd 313-line frame flips the line switch every
    frame.
  * **SNES PAL:** 1.2 DS pixels per colour cycle; the carrier moves a sixth of a cycle per line,
    a 6-line pattern that stays still from frame to frame.
  * **PlayStation (320 or 256 wide), Saturn (352 wide), Nintendo 64:** lines of the broadcast
    length, 227.5 carrier cycles, so the carrier flips every line (and every frame) and the
    artifacts form a fine pattern that a comb-filter TV separates well. The PlayStation (early
    boards) and the Saturn use the Mega Drive 2's encoder chip. The N64 uses its first boards'
    encoder, which has no filters at all, and doubles each pixel the way its video interface does,
    the second half blended with the next pixel. **PlayStation PAL** is the PAL version.
* **Cable:** RGB (no encoding), S-Video (colour softer than brightness, no rainbows), Composite,
  or RF (composite through a tuner, with optional noise and a ghost).
* **TV decoder:**
  * NTSC: the standard's colour axes, or the "red push" axes of Sony's CXA2025AS TV decoder, US
    or Japanese setting.
  * NTSC comb filter: RCA's CCD comb (1980). It separates colour from brightness using the
    signal one broadcast line earlier: a delay of exactly 227.5 carrier cycles, clocked from the
    colour burst. Half the sum is brightness and half the difference is colour, with the
    difference's content below 1 MHz put back into brightness for vertical detail. Consoles with
    broadcast-length lines separate cleanly. On the others the line above lands a little to the
    side (half a cycle on a Mega Drive), so some rainbows stay and colour edges between rows can
    leave dots.
  * PAL:
    * PAL-D, 1990s TV: the delay line nearly every PAL set had, as Philips' TDA4665 did it. Each
      line's colour is averaged with the line above's, locked to the console's lines. Colour is
      softer top to bottom and hue errors cancel out.
    * PAL-D, 1980s TV: the older glass delay line, a fixed 63.943 µs. These consoles' lines are
      longer than broadcast PAL's, so the line above's colour lands a little to the right: 2.25
      pixels on a 320-wide Mega Drive, 0.8 on a SNES.
    * PAL-S: no delay line.
* **TV controls:** Colour, Tint and Sharpness, as on the set.

Built-in presets sit under Default in the preset list. They can't be overwritten; save your own
variants under new names. The signal part of each (console, cable, TV decoder) is a real hardware
combination; the tube part (curvature, scanlines, mask, glow) is the look of that kind of set.

* **NES:** composite; RF; and the Famicom (RF only) on a Japanese TV.
* **Mega Drive 1 / Genesis:** composite; RF; PAL composite.
* **Mega Drive 2 / Genesis:** composite; composite on a US TV; composite at 256 wide (H32 games);
  S-Video (the encoder has separate Y and C outputs, which mods bring out); RF; and the Japanese
  console on a Japanese TV.
* **Mega Drive 2 (PAL):** composite; composite on a 1980s TV (glass delay line); RF; RGB SCART.
* **SNES / Super Famicom:** S-Video; composite; composite on a US TV; RF; the Super Famicom on
  a Japanese TV; and composite on a comb-filter TV.
* **SNES (PAL):** composite; composite on a PAL-S TV (no delay line); RGB SCART.
* **PlayStation:** composite; composite on a comb-filter TV; S-Video; PAL composite.
* **Saturn:** composite; S-Video.
* **Nintendo 64:** composite; composite on a comb-filter TV; S-Video.
* **Kinds of set:** Sony PVM (RGB, flat); Sony BVM (RGB, flat, finer scanlines); arcade monitor
  (RGB, curved, heavier scanlines and glow); portable TV (RF with a weak signal: snow, a ghost,
  strong curvature and a coarse mask).

What the simulation is built on:

* **Timing** (fullsnes; SNESdev wiki; SpritesMind and nesdev forum measurements):
  * Mega Drive: master clock 53.693175 MHz; carrier = master/15; pixels = master/8 (H40) or
    master/10 (H32); 3420 clocks per line.
  * SNES: master clock 21.47727 MHz; carrier = master/6; pixels = master/4; 1364 clocks per line,
    with a 1360-clock line every other frame.
  * Mega Drive PAL: master clock 53.203424 MHz; carrier = master/12; 3420 clocks per line, 313
    lines per frame (Genesis Plus GX; Kabuto's hardware notes).
  * SNES PAL: one 17.734475 MHz crystal; master clock = 6/5 of it (21.28137 MHz, 4.8 carrier
    cycles' worth); pixels = master/4; 1364 clocks per line, 312 lines (fullsnes).
  * NES (nesdev wiki): master clock 21.47727 MHz; pixels = master/4; carrier = master/6, its
    12-step colour wave stepping on both clock edges; 1364 clocks per line; every other frame a
    pixel shorter.
  * PlayStation (psx-spx; Mednafen; DuckStation): GPU clock 53.693175 MHz, carrier = /15, pixels
    /8 (320 wide) or /10 (256); lines alternate 3412 and 3413 clocks; 263 lines. PAL: 53.203425
    MHz, /12, 3405-clock lines, 314 lines.
  * Saturn (Sega's overview manual; Mednafen): 352 wide: 28.636 MHz = 8x the carrier, pixels /4,
    455 pixels per line, 263 lines.
  * N64 (n64brew wiki): video clock = 13.6x the carrier; 3094 clocks per line; 263 lines; a
    320-wide game's pixel spans 8 clocks.
* **Mega Drive 2 encoder:** the Sony CXA1645 datasheet:
  * standard colour axes and luma;
  * a chroma band-pass after the modulator;
  * the recommended 3.65 MHz luma trap on the composite output;
  * a luma delay line.
  The early PlayStations and most Saturns use the same chip.
* **Mega Drive 1 encoder:** the Sony CXA1145, which has no filters of its own, with the networks
  from Sega's schematics in place of the datasheet's delay line and band-pass, simulated as the
  circuits they are: luma through 1.2 kΩ, 100 µH ∥ 18 pF and 180 pF ∥ 1 kΩ (a 3.75 MHz notch and a
  soft roll-off); chroma through 1.2 kΩ into 12 µH ∥ 180 pF ∥ 330 Ω (a broad band-pass at 3.42
  MHz). The PAL values come from a model 1 PAL board.
* **SNES encoder:** the SMPTE 170M colour-difference filter template. The board's own filter
  values aren't documented.
* **NES:** the measured output levels (nesdev): each palette entry's low and high level, the
  burst's own levels, no setup. Its palette, for matching colours, is each entry's flat field
  decoded by the simulated TV.
* **N64 encoder:** Rohm's BA7242F (the first boards' ENC-NUS), whose datasheet has no filters.
* **TV:** demodulates against the colour burst as received, as a TV does.
  * NTSC: a 3.58 MHz luma trap and a ±0.5 MHz chroma band-pass.
  * PAL: Philips' TDA8362: a 4.43 MHz trap (Q 2), a chroma band-pass (Q 3), U/V low-pass about
    1 MHz on the standard axes. The delay lines are as above (ITU-R BT.470; TDA4665; Telefunken's
    63.943 µs glass line).
* **Exactness:** the chain is linear, so it is computed once per setting as each output sample's
  weights over nearby pixels. Applying those weights gives the simulation's result to within
  1/10000 (tested), on both OpenGL and the CPU.

Not simulated, because the hardware isn't documented well enough to do it accurately:

* the early Mega Drive 2 boards' Samsung KA2195D encoder (the blurriest ones);
* the rare PAL Mega Drive 1 VA6.8's Fujitsu MB3514 encoder;
* the later PlayStations' Sony CXA2106R and the later N64s' MAV-NUS (no datasheets);
* the PAL NES, PAL Saturn and PAL N64, whose timing isn't documented exactly enough;
* the Saturn's 320-wide mode: its carrier pattern repeats only every 488 clocks, and working out
  its weights takes seconds;
* the NES's colour emphasis bits and the slight hue shift its brighter rows have on some chip
  revisions;
* 3-line adaptive comb filters, which switch between methods per pixel (not linear).

The absolute carrier phase of the first pixel isn't documented either. It only shifts the hue
of the artifact colours.

### What draws them

What draws a filter follows Config > **Video settings**:

* an OpenGL display, or the OpenGL (Classic) or OpenGL (Compute shader) renderer: the OpenGL
  screen shaders. The CRT one does the work that depends only on DS pixels (the colour signal and
  the glow) once per frame at 256x192, so the per-pixel pass reads 19 texels instead of 43;
* the Software renderer without the OpenGL display: software versions of the same filters, on
  the CPU. They are the fallback, so the filters work without OpenGL; they look the same at
  whole-number zooms.

The software filters run in parallel: each stage is split into bands of rows across a pool of
threads, and the thread drawing the window works too. By default the pool uses half the logical
cores, 2 to 4 threads. To set the number, put `PixelGridThreads` under `[Instance0]` in
`melonDS.toml` (0 means automatic).

A screen that hasn't changed since the last frame isn't filtered again. Many DS games draw at
30 fps, and one screen is often a still menu or map. Settings, zoom and, for a console's signal,
the frame's carrier pattern have to match too; RF noise is new every frame, so with it every
frame is filtered. A screen the layout doesn't show (Top only, Bottom only) isn't filtered at all.

Per frame at 3x zoom, on 2 threads:

| Filter | Both screens changing | Top screen only |
|---|---|---|
| LCD (DSi XL stripes and blur, 4x) | 2.6 ms | about half |
| CRT, flat (curvature 0) | 3.7 ms | 2.0 ms |
| CRT, curved (the default) | 7.5 ms | 3.9 ms |
| CRT, curved, Mega Drive composite | 10.9 ms | 5.6 ms |
| CRT, curved, SNES PAL composite | 12.2 ms | 6.2 ms |

Four threads take about half as long. In TrackMania DS with the Software renderer, on a PC limited
to 4 cores, every filter and signal holds 60 fps. On 2 cores, where the filter and the emulator
compete, the CRT drops to about 45 fps. There, turn on Config > Video settings > **OpenGL display**:
the GPU then draws the filters, and the same game holds 60 fps on 2 cores with any filter and
signal. Each filter's settings window says which one is drawing it.

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
