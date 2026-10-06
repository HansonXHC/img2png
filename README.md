# img2png

English | [中文](README.zh.md)

A lossless multi-format image → PNG converter (CLI + GUI). Reads **BMP / TGA / PNM / ICO /
JPEG / PNG / GIF / QOI / WebP / TIFF / HEIF / AVIF**, writes lossless PNG — or animated PNG
(APNG) when the input is an animated GIF.

## Contents

- [Features](#features)
- [Supported input formats](#supported-input-formats)
- [Metadata](#metadata)
- [Building](#building)
- [CLI usage](#cli-usage)
- [GUI usage](#gui-usage)
- [Testing](#testing)
- [Design notes](#design-notes)
- [Known limitations](#known-limitations)
- [Third-party libraries](#third-party-libraries)

## Features

**Conversion**

- **Bit-depth matching** (default): the output PNG's color type and bit depth faithfully
  match the source image — no silent upgrades or downgrades (see the table below)
- **Animated GIF → APNG**: multi-frame GIFs become animated PNGs with frame delays, loop
  counts, per-frame transparency and disposal semantics preserved
- **PNG re-encoding**: PNG inputs are re-encoded with the current settings — pixels
  unchanged, handy for squeezing PNGs produced by other tools
- **Always lossless**: compression level and filters only affect size and speed
- **Metadata preserved**: EXIF, ICC profile, XMP, resolution and PNG text chunks travel with the
  image instead of being dropped (see [Metadata](#metadata))

**Output control**

- **Lossless compression**: zlib level 0–9 (default 9); adaptive row filtering by default
- **Timestamp preservation**: output files keep the input's creation and modification
  times, including in-place recompression

**Workflow**

- **Folder batch processing**: point it at a folder — subdirectories are scanned recursively
- **Multithreading**: thread-pool parallel conversion, defaults to the number of logical CPUs
- **Graphical front end** with drag & drop, an English/Chinese toggle and an
  overwrite-originals mode (see [GUI usage](#gui-usage))

**Build**

- **Self-contained**: zlib / libpng / libjpeg-turbo (and every other codec) ship as trimmed
  sources in `thirdparty/` — builds fully offline, no network access or manual installs
- **Cross-platform**: one CMake build for Windows / Linux / macOS; GitHub Actions builds and
  tests all three

## Supported input formats

### Bit-depth matching rules

| Input | Output PNG |
|---|---|
| BMP 1/4/8-bit (paletted) | PALETTE, bit depth preserved |
| BMP 24-bit | 8-bit RGB |
| BMP 32-bit (with alpha mask) | 8-bit RGBA |
| BMP 32-bit (no mask, BI_RGB) | 8-bit RGB (the 4th byte is undefined — not treated as alpha) |
| TGA palette (8-bit indices) | PALETTE8 (entries carrying alpha become tRNS; 15/16-bit color-map entries are expanded to 8-bit and their 1-bit alpha is dropped) |
| TGA 8-bit gray / 16-bit gray (gray+alpha) | GRAY8 / GRAY_ALPHA8 |
| TGA 24-bit / 32-bit | RGB8 / RGBA8 (a 32-bit image whose descriptor alpha size is not 8 is treated as RGB) |
| PNM grayscale (P1/P2/P4/P5) | GRAY (PBM keeps 1-bit); maxval > 255 → 16-bit, samples scaled to the full range |
| PNM color (P3/P6) | RGB; maxval > 255 → 16-bit, samples scaled to the full range |
| JPEG grayscale / color | GRAY / RGB |
| PNG | color type and bit depth preserved (1/2/4/8/16, palette tRNS, gray tRNS); RGB+tRNS expands to RGBA so the transparency survives |
| GIF | single-frame GIFs → PALETTE8 with tRNS (the palette is not narrowed to 1/2/4-bit for images with few colors); animated GIFs → APNG (see above) |
| ICO | largest entry decoded; an embedded BMP/PNG keeps its color type and depth. An AND mask that carries transparency promotes 24/32-bit entries to RGBA, and gives palette entries tRNS |
| QOI | 8-bit RGB / RGBA |
| WebP | 8-bit RGB / RGBA (lossy and lossless) |
| TIFF | bit-depth matched for contiguous images: gray 1/2/4/8/16, palette 1/2/4/8, RGB/RGBA 8/16. Other variants (CMYK, YCbCr, tiled, separate planes, float) fall back to 8-bit RGBA through the RGBA interface |
| HEIF / AVIF | 8-bit sources → 8-bit RGBA; 10/12-bit sources → 16-bit RGBA (scaled up to 16-bit, not bit-shifted) |

`--auto` optimization mode (off by default) additionally drops a fully opaque alpha channel
and collapses pure-grayscale RGB to GRAY.

### Depths that are rejected

Source depths that cannot be represented are reported as an error rather than silently
converted: **16-bit BMP**, **15/16-bit truecolor TGA** and **16-bit ICO entries**.

## Metadata

Everything that is not pixel data is carried across too — camera settings, colour profiles and
resolution are preserved rather than silently dropped.

| Source | EXIF | ICC profile | XMP | Resolution | Text |
|---|---|---|---|---|---|
| JPEG | `APP1` | `APP2` (multi-segment profiles reassembled) | `APP1` | JFIF header, else EXIF | `COM` comments |
| PNG | `eXIf` | `iCCP` | `iTXt` | `pHYs` | `tEXt` / `zTXt` / `iTXt` |
| TIFF | rebuilt from the IFD0 tags | `ICCPROFILE` tag | `XMLPACKET` tag | resolution tags | — |
| WebP | `EXIF` chunk | `ICCP` chunk | `XMP` chunk | — | — |
| HEIF | `Exif` metadata block | not exposed by libheif | XMP metadata block | — | — |
| AVIF | `exif` item | `icc` item | `xmp` item | — | — |
| BMP / TGA / PNM / ICO / QOI / GIF | these formats carry no EXIF / ICC / XMP | | | — | — |

The PNG written contains `pHYs` (resolution), `eXIf` (EXIF), `iCCP` (ICC), an `iTXt` chunk with the
`XML:com.adobe.xmp` keyword (XMP), and any text chunks a PNG source had.  Metadata preservation is
independent of `--auto`: the pixel optimizations never drop it.

Limits worth knowing:

- **EXIF is passed through verbatim** for JPEG / PNG / WebP / HEIF / AVIF, so nothing is lost —
  including the Exif sub-IFD (exposure time, F-number, ISO, ...).  For TIFF the payload is rebuilt
  from the IFD0 tags that map onto EXIF (Make, Model, Orientation, DateTime, Software, Artist,
  Copyright, resolution); TIFF's Exif sub-IFD is not rebuilt.
- **Orientation** travels as a tag and pixels are never rotated — the same thing JPEG viewers do
  with the source file.
- **HEIF**: libheif's public API has no ICC accessor, so a HEIC colour profile is not carried.
- **A libpng quirk**: an `iCCP` chunk shorter than 92 bytes is rejected on read with a "too short"
  warning, so a pathologically small compressed profile can be dropped.  Real profiles are far
  larger.
- The private `APP6`–`APP10` vendor blocks that some phone JPEGs carry (e.g. ~260 KB in a HONOR
  file) have no standard PNG equivalent and are not carried.
- Many viewers ignore `eXIf`, so a PNG's EXIF may not appear in every "image information" panel even
  though it is present in the file.

## Building

### Requirements

- CMake ≥ 3.21
- A C compiler — Windows: MinGW-w64 gcc (tested); Linux: gcc/clang; macOS: clang from the
  Xcode command-line tools
- Linux: `sudo apt install cmake ninja-build g++ qt6-base-dev` · macOS: `brew install cmake ninja qt`
- Qt 6 — optional, only needed for the GUI; the CLI still builds when Qt is absent
- No third-party packages to install: every codec is vendored in `thirdparty/`

### Steps

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 install path>"
cmake --build build
```

Outputs: `build/img2png` (CLI), `build/img2png-gui` (GUI) and `build/img2png_selftest`
(tests) — plus the `.exe` suffix on Windows.

Drop `-DCMAKE_PREFIX_PATH` to build without a Qt installation: the GUI target is skipped
automatically. Use `-DIMG2PNG_GUI=OFF` to skip it explicitly.

### Dependency resolution

Priority order: vendored `thirdparty/` → local override flags → online download
(FetchContent, with several source mirrors).

For the three dependencies that have override flags:

```bash
cmake -S . -B build -G Ninja -DIMG2PNG_GUI=OFF \
  -DZLIB_SOURCE_DIR=<zlib source dir> \
  -DLIBPNG_SOURCE_DIR=<libpng source dir> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo install dir (with include/ and lib/)>
```

`LIBJPEG_PREBUILT_DIR` may also point at an extracted official libjpeg-turbo Windows
installer package (the gcc variant); the build statically links its `lib/libjpeg.a`, so the
resulting executables have no third-party DLL dependencies.

GIF / QOI / WebP / TIFF / HEIF / AVIF support comes from the vendored `thirdparty/` sources —
nothing extra to install. libtiff is configured without its optional codecs (deflate is kept
via the vendored zlib).

### CI and releases

`.github/workflows/build.yml` builds and runs the self-test on Windows (MSYS2/MinGW),
Ubuntu and macOS. Pushing a `v*` tag runs the same three jobs and then publishes a Release
with the windows-x64, linux-x64 and macOS packages attached.

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

Behavior notes:

- Inputs may be files or **folders** (scanned recursively)
- Without `-o`: non-PNG files produce a same-name `.png` next to the input; PNG files are
  **recompressed in place**
- Output files keep the input's creation and modification times
- In `-o outdir` mode the output is flattened by file name, so identically-named files from
  different subdirectories would overwrite each other

Examples:

```bash
img2png photo.jpg                      # -> photo.png
img2png my-pictures                    # whole folder, recursively (PNGs recompressed in place)
img2png -o outdir folder *.bmp *.jpg   # output to outdir
img2png -l 6 -f paeth --auto a.ppm     # custom level / filter / auto-optimize
```

## GUI usage

`img2png-gui` (GUI subsystem — no console window; the Qt6Core/Gui/Widgets libraries must be
reachable, i.e. on `PATH` on Windows):

- Drag & drop **files or folders** onto the window (folders are scanned recursively), or use
  "Add files…" / "Add folder…"
- Compression level slider 0–9 (default 9), filter dropdown (default "adaptive"), thread count
- **UI language**: 中文/English dropdown in the top right — applies instantly and is
  remembered (default: Chinese)
- "Match source bit depth strictly" (checked by default; unchecking enables auto-optimization)
  and "keep timestamps" (default checked)
- **"Overwrite originals"** (unchecked by default):
  - when the output path equals the input path, unchecked skips the file with a notice;
    checked allows in-place overwriting (lossless, timestamps kept)
  - when checked, a non-PNG file converted into its own folder is deleted after a successful
    conversion, replaced by the same-name `.png` (noted in the log)
  - outputs going to a different directory never delete anything
- **The output-directory box accepts drag & drop**: drop a folder to fill in the path; drop a
  file to fill in its folder
- Click "Convert" — the log shows each file's result in real time

## Testing

```bash
./build/img2png_selftest            # full self-test suite
./build/img2png_selftest some.png   # verify that any PNG decodes
```

The suite generates sample files for every supported input format, converts them through the
same core code the CLI uses, then reads the PNGs back and compares pixel by pixel — 300+
assertions covering per-format decode correctness, bit-depth and color-type matching
(including sub-byte depths: 1/2/4-bit palettes and grayscale, and 16-bit gray/RGB/RGBA),
palette and tRNS handling, every filter mode, compression levels and timestamp preservation.

PNG outputs are re-read twice: once expanded to 8-bit RGBA for pixel comparison, and once at
their **native** bit depth to prove the depth really was preserved.

## Design notes

- **Lossless**: PNG output is always lossless; level and filter only affect size and speed
- **PNG re-encoding**: decoded pixels are recompressed with the new settings, unchanged
- **JPEG sources**: already lossy; decoded pixels are written to PNG without any further loss
- **Compression details**: the level goes through `png_set_compression_level()` to zlib;
  "adaptive filtering" is libpng's default behavior (try several filters per row and pick the
  best; palette images skip filtering automatically)
- **No GPU**: GPU deflate integration is costly for little gain on single images; CPU
  multithreading already saturates all cores

## Known limitations

- Source depths that cannot be represented are rejected — see
  [Depths that are rejected](#depths-that-are-rejected)
- `-o outdir` flattens the output; identically-named files from different subdirectories
  overwrite each other
- On Windows, paths are handled via the ANSI code page; Linux/macOS use UTF-8
- Windows x64 is tested locally; the Linux/macOS builds run in CI
  (`.github/workflows/build.yml`) but have not been verified on real hardware yet
- MSVC should work but is unverified

## Third-party libraries

| Library | Version | License | Source |
|---|---|---|---|
| libpng | 1.6.59 | PNG License (zlib-style) | [SourceForge libpng16/1.6.59](https://sourceforge.net/projects/libpng/files/libpng16/1.6.59/) (vendored in `thirdparty/libpng`) |
| zlib | 1.3.1 | zlib License | [madler/zlib](https://github.com/madler/zlib) (vendored in `thirdparty/zlib`) |
| libjpeg-turbo | 3.2.0 | IJG / BSD-3-Clause / zlib | [GitHub Releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/latest) (vendored in `thirdparty/libjpeg-turbo`, with a local patch — see its VENDORED.md) |
| giflib | 6.1.3 | MIT | [SourceForge giflib](https://sourceforge.net/projects/giflib/) (vendored in `thirdparty/giflib`, decode side + encoder used by the self-test) |
| libwebp | 1.6.0 | BSD-3-Clause | [GitHub Releases](https://github.com/webmproject/libwebp/releases) (vendored in `thirdparty/libwebp`) |
| libtiff | 4.7.2 | libtiff License (MIT-style) | [download.osgeo.org/libtiff](https://download.osgeo.org/libtiff/) (vendored in `thirdparty/tiff`) |
| libde265 | 1.1.3 | **LGPL-3.0** | [strukturag/libde265](https://github.com/strukturag/libde265) (vendored in `thirdparty/libde265`) |
| libheif | 1.23.5 | **LGPL-3.0** | [strukturag/libheif](https://github.com/strukturag/libheif) (vendored in `thirdparty/libheif`) |
| libavif | 1.4.2 | BSD-2-Clause | [AOMediaCodec/libavif](https://github.com/AOMediaCodec/libavif) (vendored in `thirdparty/libavif`) |
| dav1d | 1.5.4 | BSD-2-Clause | [code.videolan.org/videolan/dav1d](https://code.videolan.org/videolan/dav1d) (vendored in `thirdparty/dav1d`, hand-written CMake integration) |
| libpng APNG patch | for 1.6.59 | same as libpng | [APNG patch](https://sourceforge.net/projects/apng/files/libpng-apng/) (applied to `thirdparty/libpng`, powers GIF → APNG output) |
| Qt 6 (optional, GUI only) | 6.x | LGPL-3.0 / GPL | [qt.io](https://www.qt.io/) |

All vendored libraries are trimmed copies (docs, test assets and other architectures'
SIMD/asm code removed) that retain their original license files; see each directory
(libpng: LICENSE / LICENSES, zlib: LICENSE, libjpeg-turbo: LICENSE.md, giflib: COPYING,
libwebp: COPYING, tiff: LICENSE.md, libde265 / libheif: COPYING, libavif: LICENSE,
dav1d: COPYING).

**LGPL-3.0 note** (libde265 / libheif): vendoring the sources and statically linking them is
permitted; if you distribute modified binaries of this tool, follow the LGPL obligations for
the library parts (provide the library source or a relinkable object-file form).
