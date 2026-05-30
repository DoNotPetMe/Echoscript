# LEAD engine reference definitions

Reverse-engineering reference data for Splinter Cell: Blacklist's LEAD engine,
used to drive the mod's level editor / prop spawner.

## Critical architecture finding

`System/Blacklist.ini` confirms: **Blacklist is Unreal Engine 3**, not a standalone
LEAD-only engine. Key evidence:

```
Protocol=unreal          MapExt=unr
GameEngine=Engine.GameEngine
RenderDevice=D3DDrv.D3DRenderDevice
EditPackages=Core, Engine, Echelon, EchelonAI, EchelonCharacter, EchelonGameObject...
Class=EchelonCharacter.ESam    (Package.Class UE3 naming)
```

The **LEAD system** is a data-driven scripting / sequencing layer on top of UE3.
World actors are standard UE3 actors (placed via `UWorld::SpawnActor`), referenced in
LEAD sequences as `Repository::Unreal::Actors::UID:XXXXXXXX`. This means:

- `GObjects / GNames / GWorld` are **standard UE3** — accessible via known patterns.
- Actor spawn path is `UWorld::SpawnActor(UClass*, location, rotation)` — one function.
- `LEAD definitions.xml` types correspond to UE3 classes registered in the `Echelon*`
  packages; their `UClass*` pointers live in GObjects.
- Map files are standard `.unr` Unreal packages.

The `ObjectDumper` now tries UE3 GObjects first (fill `GOBJECTS_SIG` in
`mod/features/object_dumper.cpp` after RE), falling back to the LEAD string-anchor walk.

## Files

### Engine config

| File | What it is | How it's used |
|------|------------|---------------|
| `engine/Blacklist.ini` | Main game INI — confirms UE3, has all engine class names, render device, package list, and console key bindings | **Primary architecture reference.** Class names (`EchelonCharacter.ESam`, `Echelon.EchelonGameInfo`, etc.) match UE3 GObjects entries. |
| `engine/MapConfiguration.xml` | Map setup file (SP + Deniable Ops) — maps display name → `.unr` file path, game modes, black-box IDs | Map file path format: `_Single\{ID}\Stream\{ID}_Global_00_strm.unr`. Commented-out in this build but format is canonical. |
| `engine/StatsMaps.ini` | Complete list of all `.unr` level files — SP, MP, Coop, ADV | Source for `mod/features/map_catalog.h` (multiplayer map IDs). |
| `engine/Checkpoints.ini` | SP mission map IDs + checkpoint names for every mission | Used by the level editor map selector; source for the `map_catalog.h` checkpoint data. |

### LEAD type definitions

| File | What it is | How it's used |
|------|------------|---------------|
| `definitions.xml` | LEAD type registry — 174 types, full `Actor` hierarchy | Source of `mod/features/prop_catalog.inc` (the 62 placeable types). Regenerate with the script below. |
| `flash_ui/*.xml` | Scaleform/Flash UI layouts — menu/HUD widget trees | Reference for `Flash*` actor types and the LEAD `Command`/`Tween`/event scripting model. |
| `hud/UIData.xml` | Scaleform HUD scene registry — `sceneID` → `.gfx` asset | UI/HUD reference. |
| `hud/UIHudModes.xml` | HUD mode → list of visible HUD scenes (`eHUDMode_*`) | UI/HUD reference. |
| `hud/UIExportConfig.xml` | SWF asset build/export tooling config | Build-time tooling only. |

### Gameplay / item catalog

| File | What it is | How it's used |
|------|------------|---------------|
| `gameplay/LoadoutConfig.xml` | Player loadout presets (Campaign/Spy/Merc), slot → item `Archetype` | How items are referenced by archetype and grouped into slots. |
| `gameplay/PurchaseConfig.xml` | Full item catalog — 70 weapons, 23 gadgets, 104 armor parts + attachments/camos/upgrades, each with `Archetype`/`NetName`/`NetID` | **Most actionable for a future "specific item" spawner.** See `archetypes.tsv`. |
| `gameplay/archetypes.tsv` | Flattened index (1297 rows): `Element / NetName / NetID / Archetype / Slot / Type` | Seed for a future archetype picker (spawn a specific pistol/gadget rather than a generic type). |
| `gameplay/SC6MissionData.xml` | Mission catalog — ID → display name (all locales), map file path, briefing/debrief sequences | Source of `mod/features/map_catalog.h` SP mission display names. |
| `gameplay/MultiplayerData.xml` | Multiplayer map/mode configuration | Adversarial map reference. |
| `gameplay/cheats.xml` | Game's internal dev-cheat command set — mission state, camera, economy cheats | Console command reference. Key finding: no raw "spawn actor" console cmd — spawn goes through the engine bridge. |
| `gameplay/cheats_adv.xml` | Advanced cheats — `ADD WEAPONARCH <arch>`, `ADD VISIONMODE`, `ADD TORSO` | Weapon/gadget spawn by archetype name. Confirms the `ADD WEAPONARCH ADV_Merc_Pistol_DesertEagle` pattern for item spawning. |
| `gameplay/DefaultWaveConfig.xml` | Extraction mode default wave config | Multiplayer reference. |

### LEAD sequence examples

| File | What it is | How it's used |
|------|------------|---------------|
| `sequences/d_amman_repository.xml` | LEAD scripting repository for the D_Amman extraction map | Shows how LEAD sequences reference UE3 actors: `Repository::Unreal::Actors::UID:00000a37`. Confirms the LEAD↔UE3 bridge. Lists LEAD commands: `GetPlayerFromStart`, `GetCamera`, `GetPawn`, `CoopGetNPCFromTag`, `MPGetLocalPlayer`, `GetCheckPointID`. |
| `sequences/d_amman_seq_ld_mainsequence.xml` | Main LEAD sequence for D_Amman | Runtime LEAD scripting format: `<CommandInstance>`, `<ParameterInstance>`, `<TaskInstance>` chaining. |

## On Archetypes & NetIDs (forward-looking)

`definitions.xml` gives **types** (`Weapon`, `GameItem`, `Light`, `Mesh`, …) — the catalog
the prop spawner currently uses. `PurchaseConfig.xml` gives **archetypes**: named, data-defined
*templates* the engine instantiates from (e.g. `Pistol_PX4`, `GI_Sticky_Camera`,
`GI_ThermalGoggles`, `LightGreen`). Each archetype also carries a `NetID` — a stable hash
that is very likely the runtime key the engine uses to look the archetype up.

The `ADD WEAPONARCH ADV_Merc_Pistol_DesertEagle` console command (from `cheats_adv.xml`)
confirms that archetype names are the runtime spawn key for items. `archetypes.tsv` is the
ready-made seed list for a future archetype picker.

## Mission map ID → display name (from SC6MissionData.xml)

| Map ID | Display Name | Category |
|--------|--------------|----------|
| S_AFB  | Blacklist Zero | SP |
| S_KOB  | Safehouse | SP |
| S_BAD  | Insurgent Stronghold | SP |
| S_CON  | American Consumption | SP |
| S_NOU  | Private Estate | SP |
| S_CHE  | Abandoned Mill | SP |
| S_QOD  | Special Missions HQ | SP |
| S_FRE  | Transit Yards | SP |
| S_DET  | Detention Facility | SP |
| S_AIR  | Airstrip | SP |
| S_PAL  | American Fuel | SP |
| S_FUE  | LNG Terminal | SP |
| S_BLO  | Site F | SP |
| G01–G04 | Hawkins Seafort, Border Crossing, Hackers' Den, Billionaire's Yacht | Side (Grim) |
| H01–H04 | Opium Farm, Fish Market, Blood Diamond Mine, Dead Coast | Side (Kobin) |
| E01–E04 | Pakistani/Swiss/Egyptian/Russian Embassy | Side (Echelon) |
| C01–C04 | Smugglers Compound, Missile Plant, VORON Station, Abandoned City | Coop |
| D_Amman, D_Bratislava, D_Kigali, D_Sanaa, D_DiamondMine, D_FishMarket, D_OpiumFarm, D_BorderCross, D_HackerDen, D_SeaFort, D_Yacht | Multiplayer extraction maps | Multiplayer |

## Regenerating the prop catalog

`mod/features/prop_catalog.inc` is auto-generated from `definitions.xml` as the
transitive closure of types descending from `Actor` (excluding `*List` wrappers
and `hide="1"` scripting nodes), grouped into categories. If `definitions.xml`
is updated, regenerate the catalog rather than hand-editing it.
