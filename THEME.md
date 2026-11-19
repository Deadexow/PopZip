# MyUnzip 主题清单（共 30 套）

本次变更：删除六个「極-角色」矢量化主题与全部色块数据，改回**一个「極」主题**。
「極」使用用户提供的六人奇幻插画，**压缩后嵌入 exe**，运行时由 WIC 从内存解码绘制。
索引 29 = 極；主题总数 30 套。

## 極（索引 29）

- 素材：`assets/extreme.jpg`，1280×720，JPEG 质量 82，**101563 字节（约 99 KB）**
- 嵌入：CMake 自定义命令用 `objcopy -I binary` 生成 `theme_extreme.o`，链接进 exe；
  符号 `_binary_extreme_jpg_start` / `_binary_extreme_jpg_end`
- 解码：`LoadExtremeBitmap()` 用 WIC（`IWICImagingFactory` + `IWICStream::InitializeFromMemory`）
  转 32bppBGR，生成 DIB；首次加载后静态缓存
- 绘制：`DrawChampion()` 按 **cover**（宽/高取最大比例）铺满窗口，1× 缓存位图，之后每帧 `BitBlt`
- 叠加：26 个浅蓝漂浮光点（`FillRadialGlow` + `FillSoftCircle`）
- 配色：浅色主题（`IsLightTheme(29)`），深色文字，强调色蓝紫

体积对比：

| 版本 | 内容 | exe 体积 |
| --- | --- | --- |
| v1.41.0 | 六个矢量化主题（64 色、41.8 万色块、约 15 MB 源码） | 9.4 MB |
| v1.42.0 | 单个「極」+ 嵌入 JPEG（99 KB） | **4.45 MB** |

## 设置界面

- 共 **5 列 × 6 行 = 30 个按钮**
- 第 5 行：青空物语、星塔夜航、暮色天桥、夏日云路、夜海孤舟
- 第 6 行（末行）：林间夕桥、星漠夜穹、霓虹都会、风轮苍穹、**極**
- 行高 34、按钮高 30、起始 `y=262`，网格底部 `y=466`；保存/取消在 `y=540`，**不重叠**
- 选中后写入 `%APPDATA%\MyUnzip\settings.ini` 的 `Theme=20..29`

## 代码位置

| 内容 | 文件与位置 |
| --- | --- |
| 主题素材 | `assets/extreme.jpg`（构建时嵌入，运行时不读文件） |
| 嵌入规则 | `CMakeLists.txt` → objcopy 自定义命令 + `theme_extreme.o` 源 |
| 图片解码 | `src/sky.cpp` → `LoadExtremeBitmap()`（WIC 内存解码，静态缓存） |
| 绘制与缓存 | `src/sky.cpp` → `DrawChampion()`（cover、1× 缓存、`BitBlt`、粒子） |
| 配色 | `src/theme.h` → `GetThemePalette()` 的 `case 29`、`IsLightTheme()` |
| 主题按钮 | `src/settings_dialog.cpp` → `themeLabels[]`（30 项）、循环 `i < 30`、命中 `hit < 70` |
| 按钮布局 | `src/settings_dialog.cpp` → `ThemeButtonRect()`（5 列、行高 34、按钮高 30） |
| 静态元素 / 动画 | `src/sky.cpp` → `EnsureSky()` / `UpdateSky()` 的 `case 29`（云雾） |
| 绘制分派 | `src/sky.cpp` → `DrawSky()` 的 `case 29` → `DrawChampion()` |

原六个矢量化主题的 `theme_art_*.cpp`、`theme_art_data.h` 已删除。

## 版本

`MyUnzip v1.42.0`（`src/main.cpp` 的 `kAppVersion` 与 `CMakeLists.txt` 的 `project VERSION` 已同步）。

## 验证

- MinGW Release 构建通过，根目录 `MyUnzip.exe` 已更新（标题栏 v1.42.0，**4.45 MB**）。
- 实机截图（`PrintWindow`，仅程序窗口）：
  - `_theme29.png`：六人奇幻插画完整呈现，cover 铺满、边缘轻微裁切、无拉伸变形；发丝/武器/服饰细节清晰，画质明显优于此前的色块版；漂浮光点可见；界面文字可读
  - `_theme_settings30.png`：30 个主题按钮、5 列 × 6 行，末行含「極」，未遮挡保存/取消
- 测试后已把 `settings.ini` 恢复为用户原来的 `Theme=6`（极光雪原）。

说明：本次按用户要求改为“图片嵌入 + 运行时解码”，不再使用 GDI 色块描摹；图片体积通过 1280×720 / JPEG q82 压到 99 KB，exe 从 9.4 MB 降到 4.45 MB。