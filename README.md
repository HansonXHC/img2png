# img2png

English | [中文](README.zh.md)

A lossless multi-format image → PNG converter (CLI + GUI), based on libpng 1.6.59, libjpeg-turbo 3.2.0, giflib, libwebp and libtiff.

## Features

- **Bit-depth matching** (default): the output PNG's color type and bit depth faithfully match the source image — no silent upgrades or downgrades
- **Lossless compression**: zlib compression level 0–9 (default 9); adaptive row filtering by default
- **Folder batch processing**: point it at a folder — subdirectories are scanned recursively
- **Multithreading**: thread-pool parallel conversion, defaults to the number of logical CPUs
- **Timestamp preservation**: output files keep the input's creation and modification times (even for in-place recompression)
- **PNG re-encoding**: PNG inputs are re-encoded with the current settings — pixels unchanged, perfect for squeezing PNGs produced by other tools
- **Vendored dependencies**: zlib / libpng / libjpeg-turbo sources are bundled in `thirdparty/` — builds fully offline, no network or manual installs needed
- **Cross-platform**: one CMake build for Windows / Linux / macOS — GitHub Actions builds all three automatically (see `.github/workflows/build.yml`)

## Supported input formats & bit-depth matching rules

| Input | Output PNG |
|---|---|
| BMP 1/4/8-bit (paletted) | PALETTE, bit depth preserved |
| BMP 24-bit | 8-bit RGB |
| BMP 32-bit (with alpha mask) | 8-bit RGBA |
| BMP 32-bit (no mask, BI_RGB) | 8-bit RGB (the 4th byte is undefined — not treated as alpha) |
| TGA 8-bit gray / 24-bit / 32-bit | GRAY / RGB / RGBA |
| PNM grayscale (P1/P2/P4/P5) | GRAY (PBM keeps 1-bit), maxval>255 → 16-bit |
| PNM color (P3/P6) | RGB, maxval>255 → 16-bit |
| JPEG grayscale / color | GRAY / RGB |
| ICO | largest entry decoded; embedded BMP/PNG keeps its color type and depth |
| GIF | first frame; PALETTE8 with transparency (tRNS) |
| QOI | 8-bit RGB / RGBA |
| WebP | 8-bit RGB / RGBA (lossy & lossless) |
| TIFF | 8-bit RGBA via the RGBA interface (all photometric variants) |
| PNG | re-encoded with the current settings (lossless) |

`--auto` optimization mode (off by default): drop a fully-opaque alpha channel, collapse pure-grayscale RGB to GRAY.

## Building

### Requirements

- CMake ≥ 3.21
- A C compiler (Windows: MinGW-w64 gcc, tested; Linux: gcc/clang; macOS: clang via Xcode command-line tools)
- Linux: `sudo apt install cmake ninja-build g++ qt6-base-dev` · macOS: `brew install cmake ninja qt`
- **zlib / libpng / libjpeg-turbo are vendored in `thirdparty/`** (trimmed source copies, built together automatically) — **no network access and no manual installs required**
- Qt 6 (optional, only for the GUI; the CLI builds automatically when Qt is absent)

### Steps

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 install path>"
cmake --build build
```

Outputs: `build/img2png.exe` (CLI), `build/img2png-gui.exe` (GUI), `build/img2png_selftest.exe` (tests).

### Alternative dependency resolution (optional)

Priority order: vendored `thirdparty/` → local override flags → online download (FetchContent with multiple source mirrors).

```bash
cmake -S . -B build -G Ninja -DIMG2PNG_GUI=OFF \
  -DZLIB_SOURCE_DIR=<zlib source dir> \
  -DLIBPNG_SOURCE_DIR=<libpng source dir> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo install dir (with include/ and lib/)>
```

GIF/QOI/WebP/TIFF support is built in via vendored `thirdparty/` sources — nothing
extra to install. libtiff is configured without optional codecs (deflate is kept via
the vendored zlib).
```

`LIBJPEG_PREBUILT_DIR` may also point at an extracted official libjpeg-turbo Windows
installer package (the gcc variant); the build statically links its `lib/libjpeg.a`,
producing executables with no third-party DLL dependencies.

## CLI usage

```
img2png [-o out.png|outdir] [-l 0-9] [-f auto|none|sub|up|avg|paeth|all|fast]
        [-j N] [--auto] [--no-keep-time] file-or-folder...
```

| Option | Meaning | Default |
|---|---|---|
| `-o` | a single `.png` = exact output name; anything else = output directory (created if missing) | next to the input, same name `.png` |
| `-l 0-9` | zlib compression level | 9 |
| `-f` | row filter: auto/none/sub/up/avg/paeth/all/fast | auto (libpng adaptive) |
| `-j N` | parallel worker threads | CPU count |
| `--auto` | optimization mode (drop useless alpha / grayscale detection) | off |
| `--no-keep-time` | do not copy timestamps from the input | timestamps are kept |

- Inputs may be files or **folders** (scanned recursively)
- Without `-o`: non-PNG files produce a same-name `.png` next to the input; PNG files are **recompressed in place**
- Output files keep the input's creation and modification times

Examples:

```bash
img2png photo.jpg                      # -> photo.png
img2png my-pictures                    # convert a whole folder recursively (PNGs recompressed in place)
img2png -o outdir folder *.bmp *.jpg   # output to outdir
img2png -l 6 -f paeth --auto a.ppm     # custom level / filter / auto-optimize
```

Note: in `-o outdir` mode the output is flattened by file name; identically-named files
from different subdirectories would overwrite each other.

## GUI usage

`img2png-gui.exe` (GUI subsystem — no console window; needs the Qt6Core/Gui/Widgets DLLs on PATH):

- Drag & drop **files or folders** onto the window (folders are scanned recursively), or use "Add files…" / "Add folder…"
- Compression level slider 0–9 (default 9), filter dropdown (default "adaptive"), thread count
- **UI language**: 中文/English dropdown (top right of the window) — applies instantly and is remembered (default: Chinese)
- "Match source bit depth strictly" (checked by default; unchecking enables auto-optimization), "keep timestamps" (default checked)
- **"Overwrite originals"** (unchecked by default):
  - when the output path equals the input path, unchecked skips the file with a notice; checked allows in-place overwriting (lossless, timestamps kept)
  - when checked, a non-PNG file converted into its own folder is deleted after a successful conversion, replaced by the same-name `.png` (noted in the log)
  - outputs going to a different directory never delete anything
- **The output-directory box accepts drag & drop**: drop a folder to fill in the path; drop a file to fill in its folder
- Click "Convert" — the log shows each file's result in real time

## Tests

```bash
./build/img2png_selftest.exe             # full self-test suite
./build/img2png_selftest.exe some.png    # verify any PNG decodes
```

Covers: per-format decode correctness, bit-depth/color-type matching, pixel-by-pixel
lossless verification, all filter modes, compression levels, timestamp preservation.

## Design notes

- **Lossless**: PNG output is always lossless; level and filter only affect size and speed
- **PNG re-encoding**: decoded pixels are recompressed with the new settings, unchanged
- **JPEG sources**: already lossy; decoded pixels are written to PNG without any further loss
- **Compression details**: the level goes through `png_set_compression_level()` to zlib;
  "adaptive filtering" is libpng's default behavior (try several filters per row and pick
  the best; palette images skip filtering automatically)
- **No GPU**: GPU deflate integration is costly for little gain on single images; CPU
  multithreading already saturates all cores

## Known limitations

- 16-bit BMP input is not supported
- `-o outdir` flattens the output; identically-named files from different subdirectories overwrite each other
- On Windows, paths are handled via the ANSI code page; Linux/macOS use UTF-8
- Windows x64 is tested locally; Linux/macOS builds run in CI (`.github/workflows/build.yml`) but have not been verified on real hardware yet
- MSVC should work but is unverified

## Third-party libraries

| Library | Version | License | Source |
|---|---|---|---|
| libpng | 1.6.59 | PNG License (zlib-style) | [SourceForge libpng16/1.6.59](https://sourceforge.net/projects/libpng/files/libpng16/1.6.59/) (vendored in `thirdparty/libpng`) |
| zlib | 1.3.1 | zlib License | [madler/zlib](https://github.com/madler/zlib) (vendored in `thirdparty/zlib`) |
| libjpeg-turbo | 3.2.0 | IJG / BSD-3-Clause / zlib | [GitHub Releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/latest) (vendored in `thirdparty/libjpeg-turbo`, with a local patch — see its VENDORED.md) |
| Qt 6 (optional, GUI only) | 6.x | LGPL-3.0 / GPL | [qt.io](https://www.qt.io/) |

All vendored libraries are trimmed copies (docs, test assets and other architectures'
SIMD code removed) that retain their original license files; see each directory
(libpng: LICENSE / LICENSES, zlib: LICENSE, libjpeg-turbo: LICENSE.md).
