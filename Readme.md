# Bee Sim — Agent-Based Honeybee Colony Simulator (WIP)

Bee Sim is a fast, C/OpenGL project to explore realistic honeybee colony dynamics at scale.
It renders **thousands to tens of thousands** of bees as instanced discs (2D), runs a **fixed-step simulation**, and is progressively adding biologically-grounded behaviors (foraging loops, hive interior, recruitment) and a **hex-tiled world substrate**. It’s designed for tinkering: live parameters, debug overlays, and clean module boundaries (platform ↔ app ↔ render ↔ sim ↔ UI).

---

## Features (current snapshot)

* **C + SDL2 + OpenGL 3.3 (glad)** on Windows
* Instanced renderer (one draw call for many bees)
* Fixed-step timebase with pause/step
* Camera **pan/zoom** (zoom to cursor)
* Early **hex tile** groundwork (visualization & picking planned)
* Clean module split: `platform/`, `render/`, `sim/`, `ui/`, `config/`

Hotkeys (default):

* `Esc` quit · `Space` pause/resume · `.` step one tick while paused
* Mouse wheel / `+` `-` zoom · Right-drag / WASD pan · `0` reset camera

---

## Dev Environment Setup (Windows 10/11, MSVC)

> TL;DR: Install **Visual Studio Build Tools**, **CMake**, **vcpkg**; use CMake preset or VSCode’s CMake Tools to configure and build.

### 1) Prerequisites

* **Visual Studio 2022 Build Tools** (or full VS 2022)

  * Workloads: *Desktop development with C++*
  * Make sure the MSVC, Windows SDK, and CMake integration are ticked.
* **CMake** (3.24+) — add to `PATH` during install
* **Git**
* **vcpkg** (installed in `C:\vcpkg` is assumed here)

```powershell
# Install vcpkg (if not already)
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

### 2) Dependencies via vcpkg

From **PowerShell**:

```powershell
C:\vcpkg\vcpkg install sdl2:x64-windows glad:x64-windows
```

You should see both listed with:

```powershell
C:\vcpkg\vcpkg list
# ... sdl2:x64-windows ...
# ... glad:x64-windows ...
```

### 3) Clone the repo

```powershell
git clone https://github.com/<you>/bee-sim.git D:\bee
cd D:\bee
```

### 4) Configure with CMake (Visual Studio generator)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

> If you prefer Ninja: `winget install Ninja-build.Ninja`, then use `-G Ninja` instead of the VS generator.

### 5) Build

```powershell
cmake --build build --config Debug
# or Release
```

### 6) Run

```powershell
.\build\Debug\bee_sim.exe
```

If you see `OpenGL: 1.1.0, Vendor: Microsoft, Renderer: GDI Generic` and a blank/closing window, you’re likely on **Remote Desktop** without a proper GPU driver. See **Troubleshooting** below.

---

## VS Code Setup (optional but recommended)

1. Install extensions:

   * **CMake Tools**
   * **C/C++** (Microsoft)
2. Open the project folder.
3. Bottom status bar → **CMake: [No Kit]** → *Select a Kit* → **Visual Studio 17 2022 Release - x64** (or similar).

   * If you don’t see it: Command Palette → **CMake: Scan for Kits**.
4. Command Palette → **CMake: Configure**
   (Make sure it picks up the vcpkg toolchain. If not, set in `.vscode/settings.json`:)

   ```json
   {
     "cmake.configureArgs": [
       "-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
       "-DVCPKG_TARGET_TRIPLET=x64-windows"
     ]
   }
   ```
5. Click **Build** (CMake Tools), then **Run**.

---

## Project Layout

```
include/
  app.h           # app lifecycle API
  platform.h      # SDL/GL platform shim (init/pump/swap/shutdown)
  render.h        # renderer API + RenderView
  params.h        # Params + validation/defaults
  sim.h           # simulation state & API
  bee.h           # per-bee enums/planner hooks
  hex.h           # hex world types/helpers (in progress)
src/
  app/            # app orchestrator
  platform/       # SDL2 + glad loader, input/timing
  render/         # GL backend (instanced discs), shaders
  sim/            # SoA arrays, tick logic (motion/bounce)
  world/          # hex grid build & queries (planned/adding)
  ui/             # params panel, tile info (planned/adding)
  config/         # params defaults/validation
  main.c          # tiny entry → app_init/frame/shutdown
CMakeLists.txt
```

---

## Common Tasks

### Clean & reconfigure

```powershell
rm -r -fo build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

### Switch to Ninja (faster single-config builds)

```powershell
cmake -S . -B build-ninja -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-ninja
```

---

## Troubleshooting

**“Unable to find a valid Visual Studio instance”**
Install VS 2022 Build Tools and re-open the **x64 Native Tools Command Prompt** or restart PowerShell so updated PATHs apply.

**CMake not found**
Install CMake and tick “Add to PATH”, or reopen the shell.

**Ninja not found**
`winget install Ninja-build.Ninja` then restart your terminal; or add
`$env:Path += ";$env:LOCALAPPDATA\Microsoft\WinGet\Links"` for the current session.

**OpenGL 1.1 / GDI Generic** (window closes or no rendering)

* This happens over **Remote Desktop** or without GPU drivers.
* Fix: run locally at the machine, or use Parsec/Steam Link/any solution that preserves the hardware context; or install the proper GPU driver.
* As a fallback you can add a GL-2.1 path later, but native GL 3.3 is recommended.

**Shipping the EXE to a friend**
Include `SDL2.dll` next to the exe and ensure the **MSVC redistributable** (VS 2022) is installed on their machine.

---

## Roadmap (near-term)

* **HX-1:** Hex substrate + simple viz + click-inspect (no bee interaction)
* **B1:** Basic forager loop (outbound → harvest → return → unload → rest)
* **H0:** Hive shell (walls + entrance collisions) ✅
* **UI panel:** Live parameter editing; selection/inspection
* **Flow-fields:** Grid-based routing inside hive & for outdoors

---

## Contributing / Building Yourself

* Keep per-frame code **allocation-free**.
* Prefer **SoA** for hot sim data; **instancing** for draw.
* Use the logging macros for warnings/errors; add throttle on per-frame logs.
* PRs: small, focused (one milestone/feature), with a brief test note in the description.

---

## License

---

### Credits

* SDL2 (windowing/input), glad (OpenGL loader)
* Inspiration from honeybee behavior literature (foraging, recruitment, hygienic behavior) to ground future features
