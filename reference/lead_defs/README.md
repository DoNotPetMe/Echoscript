# LEAD engine reference definitions

Reverse-engineering reference data for Splinter Cell: Blacklist's LEAD engine,
used to drive the mod's level editor / prop spawner.

## Files

| File | What it is | How it's used |
|------|------------|---------------|
| `definitions.xml` | LEAD type registry — 174 types, full `Actor` hierarchy | Source of `mod/features/prop_catalog.inc` (the 62 placeable types). Regenerate with the script below. |
| `flash_ui/*.xml` | Scaleform/Flash UI layouts — menu/HUD widget trees (`Button`, `Label`, `ListBox`, `ListView`, `DropDown`, progress/spinner/timer) with tweens, event command bindings, focus-transfer nav | Not used by prop spawning (2D UI, not world props). Reference for the `Flash*` actor types and the LEAD `Command`/`Tween`/event scripting model. Potential basis for custom HUD injection later. |
| `hud/UIData.xml` | Scaleform HUD scene registry — `sceneID` → `.gfx` asset, `scenetype`, priority, splitscreen flags | UI/HUD only. Reference for how Scaleform scenes are registered. |
| `hud/UIHudModes.xml` | HUD mode → list of visible HUD scenes (`eHUDMode_*`) | UI/HUD only. |
| `hud/UIExportConfig.xml` | SWF asset build/export tooling config | Build-time tooling, not runtime. |
| `gameplay/LoadoutConfig.xml` | Player loadout presets (Campaign/Spy/Merc), each slot → item `Archetype` | Reference for how items are referenced by archetype and grouped into slots. |
| `gameplay/PurchaseConfig.xml` | Full item catalog — 70 weapons, 23 gadgets, 104 armor parts, 10 lights, 7 vision modes (+ attachments, camos, upgrades), each with `Archetype` / `NetName` / `NetID` and stats | **Most actionable for a future "specific item" spawner.** See `archetypes.tsv`. |
| `gameplay/archetypes.tsv` | Flattened index (1297 rows) of every catalog item: `Element / NetName / NetID / Archetype / Slot / Type` | Generated from `PurchaseConfig.xml`. Seed for a future archetype picker (spawn a *specific* pistol/gadget/light rather than a generic `Weapon`/`GameItem`/`Light` actor). |

## On Archetypes & NetIDs (forward-looking)

`definitions.xml` gives **types** (`Weapon`, `GameItem`, `Light`, `Mesh`, …) — the catalog the
prop spawner currently uses. `PurchaseConfig.xml` gives **archetypes**: named, data-defined
*templates* the engine instantiates from (e.g. `Pistol_PX4`, `GI_Sticky_Camera`,
`GI_ThermalGoggles`, `LightGreen`). Each archetype also carries a `NetID` — a stable hash that
is very likely the runtime key the engine uses to look the archetype up.

This matters for "build levels with pre-made props": once the spawn factory ABI is known
(`engine_bridge.h`), spawning by **archetype** (NetName/NetID) would place a *specific* item,
not a bare type. `archetypes.tsv` is the ready-made seed list for that.

(More XML batches land here as they're shared — command/event/parameter
definitions for the scripting layer, per-type field layouts for the engine
bridge, and asset/mesh manifests for concrete model selection.)

## Regenerating the prop catalog

`mod/features/prop_catalog.inc` is auto-generated from `definitions.xml` as the
transitive closure of types descending from `Actor` (excluding `*List` wrappers
and `hide="1"` scripting nodes), grouped into categories. If `definitions.xml`
is updated, regenerate the catalog rather than hand-editing it.
