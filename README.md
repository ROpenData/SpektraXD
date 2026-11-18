# Spektra XD

**⬇ Windows 专用版本 · Windows-only build** — Mac 用户请使用官方 [SpektraLab](https://github.com/JamesQiu2005/SpektraLab)(spektralab.app)。本仓库不含任何可运行的 Mac 程序。

![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-blue) ![GPU](https://img.shields.io/badge/GPU-Vulkan%20(NVIDIA%2FAMD)-green) ![License](https://img.shields.io/badge/license-GPL--3.0--or--later-orange)

**English** · [简体中文](README.zh-CN.md)

**Spektra XD is a Windows-only fork of [SpektraLab](https://github.com/JamesQiu2005/SpektraLab)** — a film-emulation darkroom. Open a RAW, pick a film and a paper, and get a physically modelled print: light traced through the film's dye layers and the paper's, with grain, halation, DIR couplers and the enlarger's filter pack, rendered on Vulkan with your NVIDIA or AMD GPU. **It runs only on Windows; the official Mac app lives in the upstream repository.**

## ⬇ Download (Windows x64)

**[Download Spektra XD for Windows x64 (latest)](https://github.com/ROpenData/SpektraXD/releases/latest/download/SpektraXD-2.4.2-xd-win64.zip)**
· [All releases](https://github.com/ROpenData/SpektraXD/releases)

No installer. Three steps:

1. Download the zip and unzip it to any folder.
2. Open the folder and double-click **`SpektraLabQt.exe`** — it sits at the top level, right next
   to the DLLs, `resources` and `licenses` it needs. Keep those together.
3. Import a RAW (drag it onto the window) and start.

Requires Windows 10 or later and a Vulkan-capable NVIDIA/AMD GPU driver.

## What Spektra XD adds on top of upstream

Batch and comparison tools built for working through a whole shoot:

- **Batch film / paper apply** — pick a film (or a paper), press one button, and every selected
  photo takes only that stock. Each photo keeps its own exposure, adjustments and crop, and can be
  undone individually; slide-film photos skip the paper pass automatically.
- **Effects gallery (Ctrl+G)** — the current photo rendered under all 20 films, as a large
  comparison view with a film strip below: arrow keys switch films, Enter / double-click applies,
  and the big view pans and zooms down to grain-level detail.
- **Brightness balancing** — tune one photo until it looks right, then match every selected photo's
  print brightness to it (median-luma matching, iterative convergence to ±0.05 EV; slides skipped).
- **Background thumbnails** — imported photos develop their filmstrip thumbnails on their own, on a
  second engine instance and thread; nothing needs to be clicked first.
- **Continuous wheel zoom** anchored at the cursor, on the canvas and in the gallery.
- **One-click batch export** of all selected photos to a folder, each at its own settings.
- **Performance work** — interactive edits stream full-tier reprints with the newest settings while
  the user drags; recently viewed photos reopen from an engine session cache without re-decoding;
  brightness balancing iterates at the full tier.

## What it is

- **The same engine as the Mac app**, ported to Vulkan: spectral simulation over 81 wavelengths,
  colour negative, slide and cine stocks, colour and black-and-white papers, grain and halation in
  physical units. Renders are deterministic; the Qt Quick frontend drives the engine over a serial
  worker with an exact parameter-delta protocol.
- **A subset of the Mac UI, deliberately.** The Qt Quick frontend re-implements the develop
  workflow (RAW import, film and paper, crop, adjustments, exports) and omits what is not ported.
  Five engine features remain off-Windows upstream (contrast mask, scene latitude mapping, Cineon
  DI, film edge, date back) — see `engine/PORT_PLAN.md`; the v2.4.0 Mac features (mask layers,
  double exposures, lens correction, variants) are also Mac-app side.
- **Renamed and re-iconed.** "SpektraLab" and its icon are upstream trademarks; this fork ships as
  **Spektra XD** with its own aperture icon.

## Build (Windows x64)

Validated toolchain: MinGW GCC 15.2 (WinLibs UCRT), CMake 3.25+, a Vulkan SDK (for
`glslangValidator`), Python 3.11+ (tooling only), and an NVIDIA/AMD GPU with a current driver.

```powershell
# from the repository root; dependencies land in ..\deps
.\engine\setup-libraw.ps1 -DependencyDirectory ..\deps
.\engine\build-windows.ps1 -BuildDirectory .\build\windows -LibRawSourceDirectory ..\deps\LibRaw-0.22.2

# the Qt Quick frontend (downloads the pinned Qt 6.11.3 MinGW SDK, ~278 MB)
.\windows_UI\setup-qt.ps1 -DependencyDirectory ..\deps
.\windows_UI\build.ps1 -SourceDirectory . -BuildDirectory .\build\qt -DependencyDirectory ..\deps -Jobs 8
```

`build\qt\app\SpektraLabQt.exe` is self-contained — keep the whole `app` folder together (DLLs,
QML plugins, `resources`, `licenses`). Tests: `ctest --test-dir build\windows` (16) and
`ctest --test-dir build\qt` (18), plus fixture-driven workflow tests under `windows_UI\tests`.
Details: `engine/WINDOWS.md`, `engine/WINDOWS_DESKTOP.md`, `windows_UI/README.md`.

## Credits and licence

- [SpektraLab](https://github.com/JamesQiu2005/SpektraLab) by JamesQiu — the engine, the Vulkan
  port and the Qt Quick frontend this fork builds on. **GPL-3.0-or-later.**
- [spektrafilm](https://github.com/andreavolpato/spektrafilm) by Andrea Volpato — the film
  simulation model and the measured film and paper profiles (**CC BY-SA 4.0**, kept under
  `engine/resources/profiles/` with their own attribution).
- **SpektraLab's name and icon are upstream trademarks.** This fork redistributes the code under
  the GPL and publishes under its own name, Spektra XD, with its own icon.
