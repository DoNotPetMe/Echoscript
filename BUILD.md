# Building the Blacklist Mod Injector

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| Visual Studio | 2022 | With "Desktop development with C++" workload |
| CMake | ≥ 3.20 | Add to PATH |
| Windows SDK | 10.0.20348+ | Included with VS 2022 |

## Step 1 — Fetch third-party dependencies

### ImGui (MIT licence)

```
git clone https://github.com/ocornut/imgui thirdparty/imgui
```

Copy these files from `thirdparty/imgui/` into place (they are already referenced by the CMakeLists):
- Root: `imgui.h`, `imgui.cpp`, `imgui_internal.h`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`
- `backends/imgui_impl_win32.{h,cpp}`
- `backends/imgui_impl_dx11.{h,cpp}`

The clone already puts them in the right location — nothing to move.

### MinHook (BSD-2-Clause licence)

```
git clone https://github.com/TsudaKageyu/minhook thirdparty/minhook
```

MinHook is header+lib based.  After cloning, build it with:

```
cd thirdparty/minhook
cmake -B build -A x64
cmake --build build --config Release
```

Then copy `thirdparty/minhook/build/Release/MinHook.x64.lib` to `thirdparty/minhook/lib/MinHook.x64.lib` (the CMakeLists links it automatically if you add `MinHook.x64.lib` to target_link_libraries or just add it manually in VS).

> MinHook is not required by the current code (we use raw VMT hooks), but is included in the tree for future inline hooks.  You can skip it for the first build.

## Step 2 — Configure & build

```bat
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Outputs land in `build/bin/`:
- `BlacklistInjector.exe`
- `BlacklistMod.dll`

## Step 3 — Run

1. Launch **Splinter Cell: Blacklist** (any edition, single-player only).
2. Once you reach the main menu or in-game, run `BlacklistInjector.exe` **as Administrator**.
3. The injector will wait for the game process automatically.

## In-game controls

| Key | Action |
|-----|--------|
| `INSERT` | Open / close mod menu |
| `F5` | Toggle free camera |
| `W A S D` | Move (free-cam active) |
| `Q` / `E` | Fly up / down |
| `Shift` | 3× speed boost |
| Mouse | Look around |
| `DELETE` | Unload the mod |

## Updating camera patterns

If the game is updated and the free camera stops working, open `mod/features/freecam.h` and update `CAM_MATRIX_SIG` with a new IDA/x64dbg signature that leads to the camera state write.

The pattern scanner (`mod/utils/pattern_scan.h`) accepts standard IDA-style patterns with `??` wildcards.

## Architecture notes

```
injector/
  main.cpp          — Finds Blacklist_game.exe, injects BlacklistMod.dll via
                       CreateRemoteThread + LoadLibraryW

mod/
  dllmain.cpp       — DLL entry; spawns ModThread; pumps unload loop
  hooks/
    d3d11_hook.*    — VMT-hooks IDXGISwapChain::Present; inits ImGui; ticks menu
  features/
    freecam.*       — Pattern-scans camera state; overrides it each frame
  menu/
    menu.*          — ImGui mod menu (toggle INSERT, renders overlay)
  utils/
    logger.h        — AllocConsole debug output
    memory.h        — Safe read/write, VMT hook helpers, pointer chase
    pattern_scan.h  — IDA-style byte-pattern scanner with RIP resolver
```
