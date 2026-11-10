# Building melonDS

* [Linux](#linux)
* [Windows](#windows)
* [macOS](#macos)

## Linux
The decomp workbench needs, on top of upstream melonDS: CMake 3.25 or later, Qt 6 (Qt 5 is not
supported), SQLite 3, zlib and Python 3 (code generation at build time).
The Qt Advanced Docking System is vendored in `vendor/qtads` and built with
melonDS; don't install it.

1. Install dependencies:
   * Ubuntu 26.04 (tested):
     ```bash
     sudo apt install cmake extra-cmake-modules libcurl4-gnutls-dev libpcap0.8-dev \
         libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev \
         libsqlite3-dev zlib1g-dev python3 \
         qt6-{base,base-private,multimedia,svg}-dev
     ```
   * Ubuntu 24.04: the same list. 24.04 has Qt 6.4; the workbench has only been
     built against Qt 6.10 and later, so expect to fix the odd compile error.
   * Older Ubuntu versions: not supported (no usable Qt 6).
   * Fedora: `sudo dnf install gcc-c++ cmake extra-cmake-modules SDL2-devel libarchive-devel enet-devel libzstd-devel faad2-devel sqlite-devel zlib-devel python3 qt6-{qtbase,qtbase-private,qtmultimedia,qtsvg}-devel wayland-devel` (not tested)
   * Arch Linux: `sudo pacman -S base-devel cmake extra-cmake-modules git libpcap sdl2 qt6-{base,multimedia,svg} libarchive enet zstd faad2 sqlite zlib python` (not tested)
2. Download the repository:
   ```bash
   git clone -b decomp-workbench https://github.com/sasja-san/melonDS-with-decomp
   cd melonDS-with-decomp
   ```
3. Compile:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc --all)
   ```
   The binary is `build/melonDS`.

## Windows
1. Install [MSYS2](https://www.msys2.org/)
2. Open the MSYS2 terminal from the Start menu:
   * For x64 systems (most common), use **MSYS2 UCRT64**
   * For ARM64 systems, use **MSYS2 CLANGARM64**
3. Update the packages using `pacman -Syu` and reopen the same terminal if it asks you to
4. Install git and clone the repository
   ```bash
   pacman -S git
   git clone -b decomp-workbench https://github.com/sasja-san/melonDS-with-decomp
   cd melonDS-with-decomp
   ```
5. Install dependencies:  
   Replace `<prefix>` below with `mingw-w64-ucrt-x86_64` on x64 systems, or `mingw-w64-clang-aarch64` on ARM64 systems.
   ```bash
   pacman -S <prefix>-{toolchain,cmake,SDL2,libarchive,enet,zstd,faad2,sqlite3,zlib,python}
   ```
6. Install Qt and configure the build directory
   * Dynamic builds (with DLLs)
     1. Install Qt: `pacman -S <prefix>-{qt6-base,qt6-svg,qt6-multimedia,qt6-svg,qt6-tools}`
     2. Set up the build directory with `cmake -B build`
   * Static builds (without DLLs, standalone executable)
     The workbench needs Qt 6, so upstream's `qt5-static` route no longer works.
     1. Install Qt: `pacman -S <prefix>-qt6-static`
     2. Set up the build directory with `cmake -B build -DBUILD_STATIC=ON -DCMAKE_PREFIX_PATH=$MSYSTEM_PREFIX/qt6-static`
        (not tested on this fork's current state; the vendored Qt-ADS is
        built static along with it)
7. Compile: `cmake --build build`

If everything went well, melonDS should now be in the `build` folder. For dynamic builds, you may need to run melonDS from the MSYS2 terminal in order for it to find the required DLLs.

## macOS
1. Install the [Homebrew Package Manager](https://brew.sh)
2. Install dependencies: `brew install git pkg-config cmake sdl2 qt@6 libarchive enet zstd faad2 sqlite python` (not tested)
3. Download the melonDS repository and prepare:
   ```zsh
   git clone -b decomp-workbench https://github.com/sasja-san/melonDS-with-decomp
   cd melonDS-with-decomp
   ```
4. Compile:
   ```zsh
   cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$(brew --prefix libarchive)"
   cmake --build build -j$(sysctl -n hw.logicalcpu)
   ```
If everything went well, melonDS.app should now be in the `build` directory.

### Self-contained app bundle
If you want an app bundle that can be distributed to other computers without needing to install dependencies through Homebrew, you can additionally run `
../tools/mac-libs.rb .` after the build is completed, or add `-DMACOS_BUNDLE_LIBS=ON` to the first CMake command.

## Nix (macOS/Linux)

melonDS provides a Nix flake with support for both macOS and Linux. The [Nix package manager](https://nixos.org) needs to be installed to use it.

* `nix run github:melonDS-emu/melonDS` runs upstream melonDS, without the workbench. The flake has not been updated for the workbench dependencies (SQLite, zlib), so `nix develop` here is untested.
* To get a shell for development, clone the melonDS repository and type `nix develop` in its directory.
