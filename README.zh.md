# img2png

[English](README.md) | 中文

多格式图片转 PNG 的小工具（CLI + GUI），支持 **BMP / TGA / PNM / ICO / JPEG / PNG / GIF /
QOI / WebP / TIFF / HEIF / AVIF** 输入，输出无损 PNG；输入是动图 GIF 时输出 APNG 动图。

## 目录

- [特性](#特性)
- [支持的输入格式](#支持的输入格式)
- [元数据](#元数据)
- [构建](#构建)
- [CLI 用法](#cli-用法)
- [GUI 用法](#gui-用法)
- [测试](#测试)
- [设计说明](#设计说明)
- [已知限制](#已知限制)
- [第三方库](#第三方库)

## 特性

**转换**

- **位深匹配**（默认）：输出 PNG 的色型与位深忠实对应源图，不做静默升级/降级（规则见下表）
- **动图 GIF → APNG**：多帧 GIF 转为 APNG 动图，帧延迟、循环次数、逐帧透明与 disposal
  语义完整保留
- **PNG 重编码**：PNG 输入按当前设置重新压缩，像素不变，适合给其他工具压过的 PNG"再压一遍"
- **始终无损**：压缩级别与滤镜只影响文件大小和速度
- **元数据保留**：EXIF、ICC 色彩配置、XMP、分辨率与 PNG 文本块随图一起带过去，不再丢失（见[元数据](#元数据)）

**输出控制**

- **无损压缩**：zlib 压缩级别 0–9（默认 9）；自适应行滤镜（默认，可手动指定）
- **时间戳同步**：输出文件的创建时间与修改时间与输入一致（原地覆盖同样保持）

**工作流**

- **文件夹批处理**：直接处理整个文件夹，递归扫描子目录
- **多线程**：线程池并行转换，默认线程数 = CPU 逻辑核数
- **图形界面**：支持拖拽、中英文切换、覆盖原文件模式（见 [GUI 用法](#gui-用法)）

**构建**

- **自包含**：zlib / libpng / libjpeg-turbo 及其余所有编解码器均以裁剪源码内置于
  `thirdparty/`，**完全离线可构建**，无需联网也无需手动安装
- **跨平台**：同一套 CMake 构建支持 Windows / Linux / macOS，GitHub Actions 自动构建并测试三平台

## 支持的输入格式

### 位深匹配规则

| 输入 | 输出 PNG |
|---|---|
| BMP 1/4/8 位（调色板） | PALETTE，位深原样保留 |
| BMP 24 位 | 8 位 RGB |
| BMP 32 位（有 alpha 掩码） | 8 位 RGBA |
| BMP 32 位（无掩码，BI_RGB） | 8 位 RGB（第 4 字节无定义，不当作 alpha） |
| TGA 调色板（8 位索引） | PALETTE8（色表项含 alpha 时转 tRNS；15/16 位色表项扩为 8 位，其 1 位 alpha 丢弃） |
| TGA 8 位灰度 / 16 位灰度（灰度+alpha） | GRAY8 / GRAY_ALPHA8 |
| TGA 24 位 / 32 位 | RGB8 / RGBA8（32 位但描述符中的 alpha 位数非 8 时按 RGB 处理） |
| PNM 灰度（P1/P2/P4/P5） | GRAY（PBM 保留 1 位）；maxval > 255 → 16 位，数值缩放到满量程 |
| PNM 彩色（P3/P6） | RGB；maxval > 255 → 16 位，数值缩放到满量程 |
| JPEG 灰度 / 彩色 | GRAY / RGB |
| PNG | 原色型与位深原样保留（1/2/4/8/16、调色板 tRNS、灰度 tRNS）；RGB+tRNS 扩为 RGBA 以保住透明 |
| GIF | 单帧 GIF → PALETTE8（透明色转 tRNS；调色板深度不按实际色数降到 1/2/4 位）；动图 GIF → APNG（见上文） |
| ICO | 取最大分辨率条目，内嵌 BMP/PNG 按原色型位深输出；AND 掩码携带透明时，24/32 位条目升为 RGBA，调色板条目转 tRNS |
| QOI | 8 位 RGB / RGBA |
| WebP | 8 位 RGB / RGBA（有损与无损） |
| TIFF | 连续存储的图按位深匹配：灰度 1/2/4/8/16、调色板 1/2/4/8、RGB/RGBA 8/16；其余变体（CMYK、YCbCr、分块、分离平面、浮点）回退 RGBA 接口输出 8 位 RGBA |
| HEIF / AVIF | 8 位源 → 8 位 RGBA；10/12 位源 → 16 位 RGBA（数值放大到 16 位，非位深移位） |

`--auto` 优化模式（默认关闭）会额外去掉完全无用的 alpha 通道、把纯灰度 RGB 降为 GRAY。

### 无法表达的位深

无法用 PNG 表达的源位深会明确报错，而不是静默转换：**16 位 BMP**、**TGA 15/16 位真彩**、
**ICO 16 位条目**。

## 元数据

除像素之外的其它信息也一并带过去 —— 相机参数、色彩配置、分辨率不再被静默丢弃。

| 源格式 | EXIF | ICC 色彩配置 | XMP | 分辨率 | 文本 |
|---|---|---|---|---|---|
| JPEG | `APP1` | `APP2`（多段配置自动重组） | `APP1` | JFIF 头，无则取 EXIF | `COM` 注释 |
| PNG | `eXIf` | `iCCP` | `iTXt` | `pHYs` | `tEXt` / `zTXt` / `iTXt` |
| TIFF | 由 IFD0 标签重建 | `ICCPROFILE` 标签 | `XMLPACKET` 标签 | 分辨率标签 | — |
| WebP | `EXIF` 块 | `ICCP` 块 | `XMP` 块 | — | — |
| HEIF | `Exif` 元数据块 | libheif 未提供接口 | XMP 元数据块 | — | — |
| AVIF | `exif` 项 | `icc` 项 | `xmp` 项 | — | — |
| BMP / TGA / PNM / ICO / QOI / GIF | 这些格式本身不含 EXIF / ICC / XMP | | | — | — |

输出 PNG 会写入 `pHYs`（分辨率）、`eXIf`（EXIF）、`iCCP`（ICC）、关键字为 `XML:com.adobe.xmp`
的 `iTXt`（XMP），以及源 PNG 原有的文本块。元数据保留与 `--auto` 优化正交，不会被它丢掉。

需要注意的边界：

- **EXIF 原样透传**（JPEG / PNG / WebP / HEIF / AVIF），所以不会丢任何内容 —— 包括 Exif 子 IFD
  里的曝光时间、光圈、ISO 等。TIFF 则是用 IFD0 中与 EXIF 对应的标签（Make / Model /
  Orientation / DateTime / Software / Artist / Copyright / 分辨率）重建一份，TIFF 的 Exif 子 IFD
  不重建。
- **方向标记**按标签带过去，像素不做旋转 —— 与 JPEG 查看器对源文件的处理一致。
- **HEIF**：libheif 的公开 API 没有 ICC 访问接口，所以 HEIC 的色彩配置不保留。
- **libpng 的一个行为**：读取时长度不足 92 字节的 `iCCP` 块会被判为 "too short" 丢弃，因此
  极端小的压缩配置可能丢失（真实配置远大于此）。
- 某些手机 JPEG 里的私有 `APP6`–`APP10` 厂商块（如 HONOR 文件里约 260 KB）在标准 PNG 中没有
  对应位置，不保留。
- 很多查看器会忽略 `eXIf`，因此 PNG 里虽然带着 EXIF，也未必每个"图片信息"面板都会显示。

## 构建

### 依赖

- CMake ≥ 3.21
- C 编译器：Windows 用 MinGW-w64 gcc（已测试）；Linux 用 gcc/clang；macOS 用 Xcode 命令行工具的 clang
- Linux：`sudo apt install cmake ninja-build g++ qt6-base-dev` · macOS：`brew install cmake ninja qt`
- Qt 6：可选，仅构建 GUI 需要；没有 Qt 时仍会自动只构建 CLI
- 无需安装任何第三方库：所有编解码器均内置于 `thirdparty/`

### 步骤

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 安装路径>"
cmake --build build
```

产物：`build/img2png`（CLI）、`build/img2png-gui`（GUI）、`build/img2png_selftest`（测试），
Windows 下带 `.exe` 后缀。

去掉 `-DCMAKE_PREFIX_PATH` 即可在没有 Qt 的机器上构建，GUI 目标会自动跳过；
也可用 `-DIMG2PNG_GUI=OFF` 显式跳过。

### 依赖解析方式

按优先级：仓库内置 `thirdparty/` → 本地覆盖参数 → 在线下载（FetchContent，多源回退）。

三个带覆盖参数的依赖：

```bash
cmake -S . -B build -G Ninja -DIMG2PNG_GUI=OFF \
  -DZLIB_SOURCE_DIR=<zlib 源码目录> \
  -DLIBPNG_SOURCE_DIR=<libpng 源码目录> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo 安装目录（含 include/ 与 lib/）>
```

`LIBJPEG_PREBUILT_DIR` 也可直接指向 libjpeg-turbo 官方 Windows 安装包（选 gcc 变体）解出的
目录，静态链接其 `lib/libjpeg.a`，产物无第三方 DLL 依赖。

GIF / QOI / WebP / TIFF / HEIF / AVIF 支持全部来自内置的 `thirdparty/` 源码，无需额外安装。
libtiff 已关闭其可选编解码器（deflate 走内置 zlib）。

### CI 与发布

`.github/workflows/build.yml` 在 Windows（MSYS2/MinGW）、Ubuntu、macOS 三平台构建并运行自测试。
推送 `v*` 标签会跑同样的三个 job，然后自动发布 Release，附上 windows-x64、linux-x64 与 macOS 三个包。

## CLI 用法

```
img2png [-o 输出.png|输出目录] [-l 0-9] [-f auto|none|sub|up|avg|paeth|all|fast]
        [-j 线程数] [--auto] [--no-keep-time] 文件或文件夹...
```

| 选项 | 说明 | 默认 |
|---|---|---|
| `-o` | 单个 `.png` = 精确输出名；其余情况 = 输出目录（自动创建） | 同目录同名 `.png` |
| `-l 0-9` | zlib 压缩级别 | 9 |
| `-f` | 行滤镜策略：auto/none/sub/up/avg/paeth/all/fast | auto（libpng 自适应） |
| `-j N` | 并行线程数 | CPU 核数 |
| `--auto` | 优化模式（去无用 alpha / 灰度检测） | 关 |
| `--no-keep-time` | 不复制输入文件的时间戳 | 保持时间戳 |

行为说明：

- 输入可以是文件或**文件夹**（文件夹递归扫描所有支持的图片）
- 不带 `-o` 时：非 PNG 在原目录生成同名 `.png`；PNG **原地重压缩**
- 输出文件的时间戳（创建 + 修改）与输入一致
- `-o 输出目录` 模式下输出按文件名平铺，不同子目录的同名文件会相互覆盖

示例：

```bash
img2png photo.jpg                      # -> photo.png
img2png 图片文件夹                      # 递归转换整个文件夹（PNG 原地重压缩）
img2png -o outdir 文件夹 *.bmp *.jpg   # 输出到 outdir
img2png -l 6 -f paeth --auto a.ppm     # 自定义级别/滤镜/自动优化
```

## GUI 用法

`img2png-gui`（GUI 子系统，无控制台窗口；需要 Qt6Core/Gui/Widgets 动态库可达，Windows 上即
在 `PATH` 中）：

- 拖拽**文件或文件夹**到窗口（文件夹递归扫描），或用"添加文件…"/"添加文件夹…"按钮
- 压缩级别滑块 0–9（默认 9）、滤镜下拉框（默认"自适应"）、线程数选择
- **界面语言**：窗口右上角 中文/English 下拉框切换，即时生效并自动记忆（默认中文）
- "严格匹配源图位深"（默认勾选，取消则自动优化）、"保持时间戳"（默认勾选）
- **"覆盖原文件"**（默认不勾选）：
  - 输出路径与输入文件相同时，不勾选则跳过并提示；勾选后原地覆盖（无损，时间戳保持）
  - 勾选后，同目录的非 PNG 文件转换成功后删除原文件，由同名 `.png` 取代（日志标注）
  - 输出到其他目录时不会删除原文件
- **输出目录框支持拖拽**：拖入文件夹自动填写为输出目录，拖入文件填写其所在目录；
  留空时在原目录生成
- "开始转换"后日志区实时显示每个文件的结果

## 测试

```bash
./build/img2png_selftest            # 全套自测试
./build/img2png_selftest 某文件.png  # 验证任意 PNG 可解码
```

自测试为每种支持的输入格式生成样例文件，走 CLI 用的同一套核心代码转换，再把 PNG 读回来
逐像素比对 —— 300+ 项断言，覆盖各格式解码正确性、位深与色型匹配（含子字节位深：1/2/4 位
调色板与灰度、16 位灰度/RGB/RGBA）、调色板与 tRNS 处理、全部滤镜模式、压缩级别、时间戳同步。

PNG 产物会被读回两次：一次展开为 8 位 RGBA 做像素比对，一次按**原生位深**读回，用来证明
位深确实被保留。

## 设计说明

- **无损**：PNG 输出始终无损；压缩级别与滤镜只影响文件大小和速度
- **PNG 重编码**：解码后按新设置重新压缩，像素不变
- **JPEG 源**：本身有损；解码后的像素写入 PNG 时不再有任何额外损失
- **压缩细节**：级别经 `png_set_compression_level()` 传入 zlib；"自适应滤镜"即 libpng 默认
  行为（对每行尝试多种滤镜自动选最优，调色板图自动关闭滤镜）
- **不做 GPU**：单张图上 GPU deflate 集成成本高、收益小；CPU 多线程已能跑满所有核心

## 已知限制

- 无法表达的源位深会直接报错 —— 见[无法表达的位深](#无法表达的位深)
- `-o 输出目录` 模式输出平铺，子目录同名文件会覆盖
- Windows 上路径按 ANSI 代码页处理（非 ASCII 路径在非系统代码页下可能异常）；Linux/macOS 使用 UTF-8
- Windows x64 已本地实测；Linux/macOS 由 CI 构建（`.github/workflows/build.yml`），尚未在真机验证
- MSVC 理论可用但未验证

## 第三方库

| 库 | 版本 | 许可 | 来源 |
|---|---|---|---|
| libpng | 1.6.59 | PNG License (zlib-style) | [SourceForge libpng16/1.6.59](https://sourceforge.net/projects/libpng/files/libpng16/1.6.59/)（内置于 `thirdparty/libpng`） |
| zlib | 1.3.1 | zlib License | [madler/zlib](https://github.com/madler/zlib)（内置于 `thirdparty/zlib`） |
| libjpeg-turbo | 3.2.0 | IJG / BSD-3-Clause / zlib 混合 | [GitHub Releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/latest)（内置于 `thirdparty/libjpeg-turbo`，含本地补丁，见其 VENDORED.md） |
| giflib | 6.1.3 | MIT | [SourceForge giflib](https://sourceforge.net/projects/giflib/)（内置于 `thirdparty/giflib`，仅解码侧 + 编码器用于自测试） |
| libwebp | 1.6.0 | BSD-3-Clause | [GitHub Releases](https://github.com/webmproject/libwebp/releases)（内置于 `thirdparty/libwebp`） |
| libtiff | 4.7.2 | libtiff License（MIT 风格） | [download.osgeo.org/libtiff](https://download.osgeo.org/libtiff/)（内置于 `thirdparty/tiff`） |
| libde265 | 1.1.3 | **LGPL-3.0** | [strukturag/libde265](https://github.com/strukturag/libde265)（内置于 `thirdparty/libde265`） |
| libheif | 1.23.5 | **LGPL-3.0** | [strukturag/libheif](https://github.com/strukturag/libheif)（内置于 `thirdparty/libheif`） |
| libavif | 1.4.2 | BSD-2-Clause | [AOMediaCodec/libavif](https://github.com/AOMediaCodec/libavif)（内置于 `thirdparty/libavif`） |
| dav1d | 1.5.4 | BSD-2-Clause | [code.videolan.org/videolan/dav1d](https://code.videolan.org/videolan/dav1d)（内置于 `thirdparty/dav1d`，手写 CMake 集成） |
| libpng APNG 补丁 | 针对 1.6.59 | 随 libpng 许可 | [apng 补丁](https://sourceforge.net/projects/apng/files/libpng-apng/)（已应用于 `thirdparty/libpng`，提供 GIF→APNG 动图输出） |
| Qt 6（可选，仅 GUI） | 6.x | LGPL-3.0 / GPL | [qt.io](https://www.qt.io/) |

所有内置库均为裁剪副本（去掉了与构建无关的文档/测试资源/其他架构 SIMD/汇编），
保留原许可文件；完整许可文本见各目录（libpng: LICENSE / LICENSES，zlib: LICENSE，
libjpeg-turbo: LICENSE.md，giflib: COPYING，libwebp: COPYING，tiff: LICENSE.md，
libde265 / libheif: COPYING，libavif: LICENSE，dav1d: COPYING）。

**LGPL-3.0 说明**（libde265 / libheif）：以源码形式随仓库分发、静态链接均符合 LGPL；
若你以二进制形式分发本工具的修改版，请遵循 LGPL 的对应义务（提供库部分源码或
目标文件重链接途径）。
