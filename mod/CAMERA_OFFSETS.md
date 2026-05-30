# Splinter Cell: Blacklist — camera / player coordinate internals

Target: `Blacklist_DX11_game.exe` (32-bit, DX11). Offsets below are derived
from **Paul44's Cheat Engine table** ("SC Blacklist v2.2", FearLess
Revolution). They are what the mod's free-roam implementation is built on.

## Player / camera position struct

There is **no static pointer** to this struct. The game hands it to us in a
register at a specific instruction. We AOB-scan for that instruction and
install a trampoline that copies the register out each time it runs.

| Field | Offset | Notes                          |
|-------|--------|--------------------------------|
| X     | `+0x9C`| world units (UE units ~= cm)   |
| Y     | `+0xA0`|                                |
| Z     | `+0xA4`| up axis                        |

### Capture site (Coord)

```
Blacklist_DX11_game.exe+95D6A6: F3 0F 10 86 9C 00 00 00  movss xmm0,[esi+0000009C]
Blacklist_DX11_game.exe+95D6AE: F3 0F 10 8E 50 02 00 00  movss xmm1,[esi+00000250]
```

- **AOB:** `F3 0F 10 86 9C 00 00 00 F3 0F 10 8E 50`
- At this instruction **`ESI` = struct base**.
- CE does: `newmem: mov [pCoord],esi`. We do the same with a naked trampoline
  (see `features/freecam.cpp`).

## Free-roam write site (for a future "make writes stick" upgrade)

The game re-writes the position every tick here:

```
Blacklist_DX11_game.exe+18880E: 66 0F D6 83 9C 00 00 00  movq [ebx+0000009C],xmm0
```

- **AOB:** `66 0F D6 83 9C 00 00 00 8B 46`
- At this instruction **`EBX` = struct base** (same struct as the Coord site).
- Layout seen here: `+0x9C` = X/Y (movq, 8 bytes), `+0xA4` = Z (from `[esi+08]`),
  `+0xA8` = another vec, `+0x5C` = flags (`or eax,1000`).
- CE's "Free Roam" intercepts this write and substitutes its own X/Y/Z so the
  player can be teleported/flown without the engine fighting back. If our
  simpler per-frame write (in the Present hook) jitters, port this hook.

## Camera (static, module-relative) — pitch confirmed, rest inferred

The camera setup function reads these **static** addresses each frame:

```
fld [Blacklist_DX11_game.exe+30BACB8]   ; cam X (inferred)
fld [Blacklist_DX11_game.exe+30BACBC]   ; cam Y (inferred)
fld [Blacklist_DX11_game.exe+30BACC0]   ; cam Z (inferred)
fld [Blacklist_DX11_game.exe+30BACC4]   ; pitch (labelled in CE)
mov eax,[Blacklist_DX11_game.exe+30BACDC] ; camera object ptr
```

Injection point: `Blacklist_DX11_game.exe+1AFDEEB` (AOB `56 8D 64 24 00 A1`).
These are module-relative (add the runtime module base) and ASLR-stable, so
they're a candidate path for true camera look-control later.

## Implementation status

- [x] AOB-scan + naked trampoline captures the struct base from ESI.
- [x] Per-frame write of X/Y/Z at `+0x9C/+0xA0/+0xA4` (world-axis fly).
- [ ] Camera-relative movement (needs the game yaw; only pitch is mapped).
- [ ] Free-roam write-site hook (if per-frame writes jitter).
- [ ] Camera look-control via the static `+30BAC..` block.
