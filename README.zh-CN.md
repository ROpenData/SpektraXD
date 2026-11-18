# Spektra XD

**⬇ Windows 专用版本 · Windows-only build** — Mac 用户请使用官方 [SpektraLab](https://github.com/JamesQiu2005/SpektraLab)(spektralab.app)。本仓库不含任何可运行的 Mac 程序。

![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-blue) ![GPU](https://img.shields.io/badge/GPU-Vulkan%20(NVIDIA%2FAMD)-green) ![License](https://img.shields.io/badge/license-GPL--3.0--or--later-orange)

[English](README.md) · **简体中文**

**Spektra XD 是 [SpektraLab](https://github.com/JamesQiu2005/SpektraLab) 的 Windows 专用分叉**——一个胶片模拟暗房。打开一张 RAW，选好胶片和相纸，得到一张按物理过程建模的照片：光线穿过胶片与相纸各自的染料层，颗粒、光晕、DIR 成色剂、放大机滤色一样不少，由 Vulkan 在你的 NVIDIA / AMD 显卡上渲染。**它只能在 Windows 上运行；官方 Mac 应用在上游仓库。**

## ⬇ 下载(Windows x64)

**[下载 Spektra XD · Windows x64（最新版）](https://github.com/ROpenData/SpektraXD/releases/latest/download/SpektraXD-2.4.2-xd-win64.zip)**
· [全部版本列表](https://github.com/ROpenData/SpektraXD/releases)

无需安装，三步即用：

1. 下载 zip，解压到任意文件夹。
2. 打开文件夹，双击 **`SpektraLabQt.exe`**——它就在顶层，旁边就是它需要的 DLL、`resources` 和 `licenses`，请保持它们在一起。
3. 把一张 RAW 拖进窗口，开始修图。

需要 Windows 10 及以上，以及一块支持 Vulkan 的 NVIDIA / AMD 显卡驱动。

## Spektra XD 在上游基础上增加的功能

为整批照片的工作流而做的批量与对比工具：

- **批量应用胶片 / 相纸**——选好一款胶片（或相纸），按一个按钮，所有选中的照片只更换这一项。每张照片保留自己的曝光、调整与裁切，可分别撤销；正片照片在相纸环节自动跳过。
- **效果总览（Ctrl+G）**——当前照片渲染全部 20 款胶片，呈现在一张大图上，底部是胶片条：方向键切换，Enter / 双击应用，大图可缩放平移到颗粒级别的细节。
- **均衡亮度**——把一张照片调到满意，然后让所有选中照片的相纸亮度自动匹配它（中位亮度匹配，迭代收敛到 ±0.05 EV；正片跳过）。
- **后台缩略图**——导入的照片在独立的第二个引擎实例与线程上自行生成胶片条缩略图，无需先点击任何照片。
- **滚轮连续缩放**——以光标为锚点，主画布与效果总览大图均可用。
- **一键批量导出**——把所有选中的照片按各自的设置导出到文件夹。
- **性能优化**——拖动调整时以全分辨率重印流式预览最新设置；最近看过的照片从引擎会话缓存直接恢复，无需重新解码；均衡亮度在全分辨率档迭代。

## 它是什么

- **与 Mac 版同一颗引擎**，移植到 Vulkan：81 个波长的光谱模拟，彩色负片、反转片、电影片，彩色与黑白相纸，颗粒与光晕按物理尺度计算。渲染完全确定；Qt Quick 前端通过一条串行工作线程与精确的参数增量协议驱动引擎。
- **刻意保持为 Mac 界面的子集。** Qt Quick 前端实现了冲洗工作流（RAW 导入、胶片与相纸、裁切、调整、导出），未移植的部分不做。五个引擎功能在上游仍不支持 Windows（反差蒙版、场景宽容度映射、Cineon 数字中间片、片边、日期后背）——见 `engine/PORT_PLAN.md`；v2.4.0 的 Mac 新功能（蒙版图层、双重曝光、镜头校正、变体）也仅存在于 Mac 应用。
- **改名并更换图标。**“SpektraLab”名称与图标是上游商标；本分叉以 **Spektra XD** 之名发布，使用自己的光圈图标。

## 构建（Windows x64）

已验证的工具链：MinGW GCC 15.2（WinLibs UCRT）、CMake 3.25+、Vulkan SDK（提供 `glslangValidator`）、Python 3.11+（仅工具使用），以及一块驱动较新的 NVIDIA / AMD 显卡。

```powershell
# 在仓库根目录运行；依赖下载到 ..\deps
.\engine\setup-libraw.ps1 -DependencyDirectory ..\deps
.\engine\build-windows.ps1 -BuildDirectory .\build\windows -LibRawSourceDirectory ..\deps\LibRaw-0.22.2

# Qt Quick 前端（下载钉住版本的 Qt 6.11.3 MinGW SDK，约 278 MB）
.\windows_UI\setup-qt.ps1 -DependencyDirectory ..\deps
.\windows_UI\build.ps1 -SourceDirectory . -BuildDirectory .\build\qt -DependencyDirectory ..\deps -Jobs 8
```

`build\qt\app\SpektraLabQt.exe` 是自包含的——请保持整个 `app` 文件夹一起移动（DLL、QML 插件、`resources`、`licenses`）。测试：`ctest --test-dir build\windows`（16 项）与 `ctest --test-dir build\qt`（18 项），另有 `windows_UI\tests` 下以真实 RAW 夹具驱动的工作流测试。详见 `engine/WINDOWS.md`、`engine/WINDOWS_DESKTOP.md` 与 `windows_UI/README.md`。

## 致谢与许可

- [SpektraLab](https://github.com/JamesQiu2005/SpektraLab)，作者 JamesQiu——本分叉所依赖的引擎、Vulkan 移植与 Qt Quick 前端。**GPL-3.0-or-later。**
- [spektrafilm](https://github.com/andreavolpato/spektrafilm)，作者 Andrea Volpato——胶片模拟模型与实测胶片、相纸配置（**CC BY-SA 4.0**，保留于 `engine/resources/profiles/`，附带各自的署名要求）。
- **“SpektraLab”名称与图标是上游商标。**本分叉依据 GPL 再分发代码，并以自己的名称 Spektra XD 与自己的图标发布。
