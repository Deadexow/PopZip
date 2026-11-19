<p align="center">
  <img src="icon.png" width="96" alt="PopZip" />
</p>

<h1 align="center">PopZip</h1>

<p align="center">轻量、纯 Win32 的 Windows 压缩 / 解压工具 · v1.55.0</p>

<p align="center">开发者：<b>ChitoseVulpes</b> · 项目主页：<a href="https://github.com/Deadexow/PopZip">github.com/Deadexow/PopZip</a></p>

---

## 简介

PopZip 是一款面向 Windows 的单文件压缩 / 解压工具。界面完全使用原生 Win32（`windows.h` / `dwmapi.h`）自绘，无边框深色 UI，内置 7-Zip 与 ZPAQ 引擎，**不依赖外部 `7za.exe` / `7z.dll`**。

- 解压 / 压缩全程进程内完成
- 20 套动态背景主题（暗夜墨韵、樱吹花落、星河璀璨、极光雪原、赛博霓虹…）
- 压缩包内容预览（explorer 式）、包内搜索、拖出/拖入追加
- 资源管理器右键菜单（HKCU，可一键注册 / 取消）

## 功能

### 解压
- 格式：`zip` `zipx` `7z` `rar`（RAR4 / RAR5，仅解压）`tar` `gz` `tgz` `tar.xz` `xz` `iso` `zpaq`
- 分卷：`name.z01/z02…`、`name.001/002…`（ZIP / 7Z 分卷自动合并，后台执行不卡界面）
- 密码：自动检测加密包；支持 ZIP AES-256 / 7Z 加密头
- 同名冲突：覆盖 / 跳过 / 保留两者（重命名旧文件）
- 安全：拒绝 `..`、盘符、UNC 路径（Zip Slip）；解压炸弹阈值 64 GB / 200000 项
- 任务队列：多个解压任务排队执行；命令行可一次传入多个包
- 大包优化：归档句柄缓存，预览刷新 / 双击条目不再重复打开整包

### 压缩
- 格式：`ZIP` `7Z` `TAR` `TGZ` `TAR.XZ` `ZPAQ`（不支持创建 RAR）
- 多来源、可编辑输出路径
- 密码：ZIP（AES-256）/ 7Z（加密头）
- 分卷：不分卷 / 10 MB / 50 MB / 100 MB / 300 MB / 500 MB / 1 GB / 2 GB，按 `name.ext.001`、`.002`… 切分
- 记住上次的压缩级别 / 方法 / 线程数 / 分卷大小

### 预览
- explorer 式：左侧条目列表，右侧图片 / 音视频预览
- 快捷键：`↑/↓` 选择、`Home/End`、`Enter` 打开、`Backspace` 返回上级、`Esc` 关闭
- 包内搜索：工具栏输入即时过滤
- 右键：打开 / 删除 / 重命名 / 解压到… / 复制路径
- 测试压缩包：逐条校验 CRC，列出损坏条目
- 拖出条目到资源管理器；从资源管理器拖入文件追加到当前目录
- 支持分卷包预览

### 设置
- 解压后关闭窗口、预览音视频自动播放
- 默认压缩格式、主题
- 用户协议、开发者信息、GitHub 主页
- 注册 / 取消资源管理器右键菜单

## 主题

当前共 20 套：暗夜墨韵、皎日白昼、星河璀璨、深渊黑洞、樱吹花落、蔚蓝地球、极光雪原、鎏金雨夜、流萤夏夜、落叶秋径、深海气泡、赛博霓虹、矩阵代码、星云旋涡、奶油午后、月光白橘、星空薄荷、月球背面、电路板纹、深海鲸落。

主题采用程序化绘制（GDI），支持动画；设置窗口 4×5 网格选择，保存在 `%APPDATA%\PopZip\settings.ini`。

## 命令行

```bat
PopZip.exe                     :: 启动主界面
PopZip.exe "a.zip"             :: 解压到同名文件夹
PopZip.exe "a.zip" "b.zip"     :: 第二个及以后的任务排队
PopZip.exe -extract-here "a.zip"  :: 解压到压缩包所在目录
PopZip.exe -extract-to   "a.zip"  :: 解压到同名文件夹
PopZip.exe -compress     "D:\dir" :: 打开压缩设置
PopZip.exe -preview      "a.zip" [password] :: 打开预览窗口
```

## 资源管理器右键菜单

在 **设置 → 注册右键菜单** 开启（写入 `HKCU\Software\Classes\SystemFileAssociations\<ext>\shell` 与 `Directory\shell`，不修改系统全局、不需要管理员权限）。

- 压缩包文件：`PopZip 解压到当前文件夹` / `PopZip 解压到同名文件夹`
- 文件夹：`PopZip 压缩为 ZIP`

> Windows 11 的第三方传统菜单项位于右键菜单的 **「显示更多选项」**（或 `Shift+F10`）中。

## 构建

环境：MinGW-w64（GCC 16.2.0）+ CMake 3.20+。

```bat
cmake -S . -B build_release -G "MinGW Makefiles" ^
  -DCMAKE_TOOLCHAIN_FILE=.cmake-mingw-toolchain.cmake ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j 4
```

产物：`build_release\PopZip.exe`（静态链接，单文件）。

> 若 windres 报 `cannot execute 'cc1'`，`CMakeLists.txt` 已通过 `CMAKE_RC_FLAGS --preprocessor-arg=-B<gcc libexec>` 处理；自定义工具链时请保留该路径。

## 目录结构

```
.
├─ src/                   # 全部应用源码（Win32 自绘 UI、引擎封装）
├─ third_party/
│  ├─ 7zip-src/           # 7-Zip 源码（含 lib7zengine.a）
│  └─ zpaq/               # ZPAQ 官方源码 7.15
├─ assets/                # 主题素材
├─ icon.png / icon.ico    # 应用图标
├─ popzip.rc              # 图标资源脚本
├─ upload.bat             # 一键提交 / 推送 GitHub
├─ CMakeLists.txt
└─ PopZip.exe             # 构建产物
```

## 已知限制

- RAR 仅支持解压，不支持创建
- 分卷压缩包不支持条目删除 / 重命名
- 删除 / 重命名采用「解包 → 修改 → 按原格式重压」，超大包耗时较长
- 不支持 per-monitor DPI 感知（固定像素布局）
- 仅支持 Windows；未提供 Linux / macOS 版本

## 用户协议

使用本软件前请阅读软件内 **设置 → 用户协议**。要点：

- 所有文件均在本机处理，不收集、不上传数据；密码仅保存在内存
- 软件按“现状”提供，请自行备份重要数据
- 内置 7-Zip（www.7-zip.org）与 zpaq（mattmahoney.net）开源实现，遵循其各自许可

## 致谢

- 7-Zip — Igor Pavlov
- ZPAQ — Matt Mahoney
