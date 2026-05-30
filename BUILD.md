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

### nlohmann/json (MIT licence) — required by the level editor

The level editor saves/loads levels as JSON. Fetch the single-header release:

```
curl -L --create-dirs -o thirdparty/json/nlohmann/json.hpp ^
  https://github.com/nlohmann/json/releases/latest/download/json.hpp
```

This lands at `thirdparty/json/nlohmann/json.hpp`, which `mod/CMakeLists.txt` already
adds to the include path. No build step — it's header-only.

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
| `F6` | Toggle level editor |
| `DELETE` | Unload the mod |

## Level editor (prop spawner)

Open the menu (`INSERT`) and expand **Level Editor**. It is seeded with the **62 placeable
LEAD types** extracted from the game's `definitions.xml` (the full `Actor` hierarchy:
`Mesh`, `Light`, `Door`, `ParticleSpawner`, cameras, volumes, AI, etc.), grouped by category.

| Panel | What it does |
|-------|--------------|
| **Discovery (Object Dumper)** | Locates the in-memory LEAD type registry and resolves catalog entries to live type pointers. Tune the node offsets and click *Dump types* until names print to the console. |
| **Prop Browser** | Search/filter the catalog; entries marked `[live]` have a resolved type node. *Spawn at camera* places the selected type in front of you. |
| **Placed Props** | Lists everything you've placed (red = not live). Select to edit; delete or clear. |
| **Transform** | Translate/Rotate/Scale radio + numeric drag fields. With a prop selected, an on-screen 3D gizmo lets you drag the translate handles. |
| **Save / Load** | Persists placements to JSON (`blacklist_level.json` by default) and re-spawns on load. |

### Architecture insight: Blacklist is Unreal Engine 3

`System/Blacklist.ini` (from the game dump) confirms: **Blacklist runs on standard UE3.**
`Protocol=unreal`, `MapExt=unr`, `GameEngine=Engine.GameEngine`, edit packages
`Core / Engine / Echelon / EchelonGameObject / EchelonCharacter`. The LEAD system is a
scripting layer on top of UE3, not a replacement.

This means:
- `GObjects / GNames / GWorld` exist as standard UE3 structures.
- Actor classes use UE3 `Package.ClassName` format: `EchelonCharacter.ESam`, `Echelon.AIManager`.
- The spawn path is `UWorld::SpawnActor(UClass*, FVector, FRotator)` — one virtual function.
- Map files are `.unr` Unreal packages.

The Discovery panel in-game shows whether the UE3 GObjects path or the LEAD string-anchor
fallback is active.

### Reverse-engineering bring-up (required for live spawning)

The editor framework, catalog, gizmo, and save/load all work immediately. **Actual spawning
requires filling in game-specific signatures/offsets.** Recommended order (lowest → highest risk):

1. **Object Dumper / GObjects first** (read-only).
   - Fill `GOBJECTS_SIG` in `mod/features/object_dumper.cpp` with an IDA/x64dbg pattern
     that resolves to the global `TArray<FObjectItem> GObjects`. When correct, the
     Discovery panel shows "UE3 GObjects path ACTIVE".
   - Click *Dump types* — recognisable class names (`Actor`, `Mesh`, `EchelonGameObject.Mesh`)
     should print to the console. Click *Resolve catalog* — browser entries flip to `[live]`.
   - Fallback: if GObjects is unknown, tune the LEAD node-layout offsets in the Discovery
     panel until the LEAD string-anchor path walks recognisable names.
2. **Spawn factory.** Fill `SPAWN_BY_TYPE_SIG` in `mod/features/engine_bridge.h` with the
   signature of `UWorld::SpawnActor` (standard UE3 virtual at a predictable vtable slot).
   Verify the function prologue in a disassembler before the first call. Because LEAD types
   map to UE3 classes, this is **one** factory parameterised by `UClass*`.
3. **Transform / destroy.** Fill `SET_TRANSFORM_SIG` and `DESTROY_ACTOR_SIG`; drive via the
   numeric fields first.
4. **Gizmo.** Validated against the freecam view matrix; if the handles are mis-projected,
   flip the handedness assumptions in `freecam.cpp::GetViewProjection`.

All raw engine calls are wrapped in `__try/__except`, so a wrong signature reports an error
instead of crashing the game.

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
    freecam.*            — Pattern-scans camera state; overrides it each frame
    leveleditor.*        — Orchestrates the prop spawner: placed-prop list,
                           spawn/transform/delete, menu UI, gizmo, save/load
    engine_bridge.*      — RE seam: spawn/transform/destroy fn pointers + GWorld,
                           resolved by signature; raw calls __try/__except-guarded
    object_dumper.*      — Locates UClass* via UE3 GObjects (preferred) or LEAD
                           string-anchor fallback; resolves live type-node pointers
    map_catalog.h        — Hardcoded SP/MP map ID → display name table (from
                           SC6MissionData.xml + Checkpoints.ini)
    prop_database.*      — 62-type catalog seeded from prop_catalog.inc
    prop_catalog.inc     — AUTO-GENERATED from definitions.xml (Actor descendants)
    gizmo.*              — World-to-screen + on-screen translate handles
    level_serialization.* — JSON save/load (nlohmann/json)
  menu/
    menu.*          — ImGui mod menu (toggle INSERT, renders overlay)
  utils/
    logger.h        — AllocConsole debug output
    memory.h        — Safe read/write, VMT hook helpers, pointer chase
    pattern_scan.h  — IDA-style byte-pattern scanner with RIP resolver
```
