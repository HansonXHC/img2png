# img2png v1.0.0

[English](README.md) | 中文

多格式图片转 PNG 的小工具（CLI + GUI），支持 BMP / TGA / PNM / ICO / JPEG / PNG 输入。
基于 libpng 1.6.59 与 libjpeg-turbo 3.2.0，全部转换**无损**。

## 特性

- **位深匹配**：输出 PNG 的色型与位深忠实对应源图，不做静默升级/降级
- **无损压缩**：zlib 压缩级别 0–9（默认 9）；自适应行滤镜（默认，可手动指定）
- **文件夹批处理**：直接处理整个文件夹，递归扫描子目录
- **多线程**：线程池并行转换，默认线程数 = CPU 逻辑核数
- **时间戳同步**：输出文件的创建时间与修改时间与输入一致（原地覆盖同样保持）
- **PNG 重编码**：PNG 输入按当前设置（级别/滤镜）重新压缩，像素不变，适合给老工具压过的 PNG "再压一遍"
- 依赖自动下载：配置时自动拉取 zlib / libpng / libjpeg-turbo 源码构建，也可用本地副本离线构建

## 支持的输入格式与位深匹配规则

| 输入 | 输出 PNG |
|---|---|
| BMP 1/4/8 位（调色板） | PALETTE，位深原样保留 |
| BMP 24 位 | 8 位 RGB |
| BMP 32 位（有 alpha 掩码） | 8 位 RGBA |
| BMP 32 位（无掩码，BI_RGB） | 8 位 RGB（第 4 字节无定义，不当作 alpha） |
| TGA 8 位灰度 / 24 位 / 32 位 | GRAY / RGB / RGBA |
| PNM 灰度（P1/P2/P4/P5） | GRAY（PBM 保留 1 位），maxval>255 → 16 位 |
| PNM 彩色（P3/P6） | RGB，maxval>255 → 16 位 |
| JPEG 灰度 / 彩色 | GRAY / RGB |
| ICO | 取最大分辨率条目，内嵌 BMP/PNG 按原色型位深输出 |
| PNG | 按当前设置重新编码（无损） |

`--auto` 优化模式（默认关闭）：去掉完全无用的 alpha 通道、纯灰度图降为 GRAY。

## 构建

### 依赖

- CMake ≥ 3.21
- C 编译器（在 MinGW-w64 gcc 13 上开发测试；MSVC 未测试）
- **zlib / libpng 1.6.59 / libjpeg-turbo 3.2.0 已内置于 `thirdparty/`**（裁剪后的源码副本，
  配置时自动一起构建），**无需联网、无需手动安装**
- Qt 6（可选，仅构建 GUI 需要；没有 Qt 时自动只构建 CLI）

### 步骤

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt6 安装路径>"
cmake --build build
```

产物：`build/img2png.exe`（CLI）、`build/img2png-gui.exe`（GUI）、`build/img2png_selftest.exe`（测试）。

### 其他依赖解析方式（备选）

按优先级：仓库内置 `thirdparty/` → 本地覆盖参数 → 在线下载（FetchContent，多源回退）。

```bash
cmake -S . -B build -G Ninja \
  -DIMG2PNG_GUI=OFF \
  -DZLIB_SOURCE_DIR=<zlib 源码目录> \
  -DLIBPNG_SOURCE_DIR=<libpng 源码目录> \
  -DLIBJPEG_PREBUILT_DIR=<libjpeg-turbo 安装目录（含 include/ 与 lib/）>
```

`LIBJPEG_PREBUILT_DIR` 也可直接指向 libjpeg-turbo 官方 Windows 安装包（选 gcc 变体）解出
的目录，静态链接其 `lib/libjpeg.a`，产物无 DLL 依赖。

## CLI 用法

```
img2png [-o 输出.png|输出目录] [-l 0-9] [-f auto|none|sub|up|avg|paeth|all|fast]
        [-j 线程数] [--auto] [--no-keep-time] 文件或文件夹...
```

| 选项 | 说明 | 默认 |
|---|---|---|
| `-o` | 单个 `.png` = 精确输出名；其余情况 = 输出目录（自动创建） | 同目录同名 `.png` |
| `-l 0-9` | zlib 压缩级别 | 9 |
| `-f` | 行滤镜策略 | auto（libpng 自适应） |
| `-j N` | 并行线程数 | CPU 核数 |
| `--auto` | 优化模式（去无用 alpha / 灰度检测） | 关 |
| `--no-keep-time` | 不复制输入文件的时间戳 | 保持时间戳 |

- 输入可以是文件或**文件夹**（文件夹递归扫描所有支持的图片）
- 不带 `-o` 时：非 PNG 在原目录生成同名 `.png`；PNG **原地重压缩**
- 输出文件的时间戳（创建 + 修改）与输入一致

示例：

```bash
img2png photo.jpg                      # -> photo.png
img2png 图片文件夹                      # 递归转换整个文件夹（PNG 原地重压缩）
img2png -o outdir 文件夹 *.bmp *.jpg   # 输出到 outdir
img2png -l 6 -f paeth --auto a.ppm     # 自定义级别/滤镜/自动优化
```

注意：`-o 输出目录` 模式下输出按文件名平铺，不同子目录的同名文件会相互覆盖。

## GUI 用法

`img2png-gui.exe`（GUI 子系统，无控制台窗口；运行需要 Qt6Core/Gui/Widgets DLL 在 PATH 中）：

- 拖拽**文件或文件夹**到窗口（文件夹递归扫描），或用"添加文件…"/"添加文件夹…"按钮
- 压缩级别滑块 0–9（默认 9）、滤镜下拉框（默认"自适应"）、线程数选择
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
./build/img2png_selftest.exe             # 全套自测试
./build/img2png_selftest.exe 某文件.png   # 验证任意 PNG 可解码
```

自测试覆盖：各格式解码正确性、位深/色型匹配、逐像素无损校验、全部滤镜模式、
压缩级别、时间戳同步。

## 设计说明

- **无损**：PNG 输出始终无损；压缩级别与滤镜只影响文件大小和速度
- **PNG 重编码**：解码后按新设置重新压缩，像素不变
- **JPEG 源**：本身有损；解码后的像素写入 PNG 时不再有任何额外损失
- **压缩细节**：级别经 `png_set_compression_level()` 传入 zlib；"自适应滤镜"即
  libpng 默认行为（对每行尝试多种滤镜自动选最优，调色板图自动关闭滤镜）

## 已知限制

- 16 位 BMP 输入不支持
- `-o 输出目录` 模式输出平铺，子目录同名文件会覆盖
- Windows ANSI 代码页处理路径（非 ASCII 路径在非系统代码页下可能异常）
- 仅在 MinGW-w64 (gcc 13) 上测试过；MSVC 理论可用但未验证

## 第三方库

| 库 | 版本 | 许可 | 来源 |
|---|---|---|---|
| libpng | 1.6.59 | PNG License (zlib-style) | [SourceForge libpng16/1.6.59](https://sourceforge.net/projects/libpng/files/libpng16/1.6.59/)（内置于 `thirdparty/libpng`） |
| zlib | 1.3.1 | zlib License | [madler/zlib](https://github.com/madler/zlib)（内置于 `thirdparty/zlib`） |
| libjpeg-turbo | 3.2.0 | IJG / BSD-3-Clause / zlib 混合 | [GitHub Releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/latest)（内置于 `thirdparty/libjpeg-turbo`，含本地补丁，见其 VENDORED.md） |
| Qt 6（可选，仅 GUI） | 6.x | LGPL-3.0 / GPL | [qt.io](https://www.qt.io/) |

三个内置库均为裁剪副本（去掉了与构建无关的文档/测试资源/其他架构 SIMD），保留原许可
文件；完整许可文本见各目录（libpng: LICENSE / LICENSES，zlib: LICENSE，
libjpeg-turbo: LICENSE.md）。
