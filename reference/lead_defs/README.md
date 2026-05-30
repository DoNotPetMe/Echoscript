# LEAD engine reference definitions

Reverse-engineering reference data for Splinter Cell: Blacklist's LEAD engine,
used to drive the mod's level editor / prop spawner.

## Files

| File | What it is | How it's used |
|------|------------|---------------|
| `definitions.xml` | LEAD type registry — 174 types, full `Actor` hierarchy | Source of `mod/features/prop_catalog.inc` (the 62 placeable types). Regenerate with the script below. |
| `flash_ui/*.xml` | Scaleform/Flash UI layouts — menu/HUD widget trees (`Button`, `Label`, `ListBox`, `ListView`, `DropDown`, progress/spinner/timer) with tweens, event command bindings, focus-transfer nav | Not used by prop spawning (2D UI, not world props). Reference for the `Flash*` actor types and the LEAD `Command`/`Tween`/event scripting model. Potential basis for custom HUD injection later. |

(More XML batches land here as they're shared — command/event/parameter
definitions for the scripting layer, per-type field layouts for the engine
bridge, and asset/mesh manifests for concrete model selection.)

## Regenerating the prop catalog

`mod/features/prop_catalog.inc` is auto-generated from `definitions.xml` as the
transitive closure of types descending from `Actor` (excluding `*List` wrappers
and `hide="1"` scripting nodes), grouped into categories. If `definitions.xml`
is updated, regenerate the catalog rather than hand-editing it.
