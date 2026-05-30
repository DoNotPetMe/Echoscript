# Reverse-Engineering Bring-Up (32-bit Blacklist)

Freecam and the prop spawner are **frameworks with the game-specific data left
blank**. To switch them on you must find a few addresses/signatures *inside your
own copy of the 32-bit game* and paste them into the source. Nobody can do this
remotely — the addresses are specific to your game's exact build.

> **Be realistic about the difficulty.** This is genuine reverse-engineering. It
> takes patience, a disassembler/debugger, and some trial and error. The freecam
> (one camera pointer) is by far the easiest target — start there. The spawner
> (GObjects + engine functions) is considerably harder; treat it as a stretch
> goal.

Tools you'll want (all free):
- **Cheat Engine** — easiest way to locate the camera in memory. https://cheatengine.org
- **x32dbg** (the 32-bit half of x64dbg) — to read the instruction bytes for a
  signature. https://x64dbg.com

---

## Part 1 — Freecam (recommended first target)

The mod needs **one pointer**: the address of the camera struct, whose layout is
(see `mod/features/freecam.h`):

```
offset 0x00  view matrix  (16 floats, 4x4)
offset 0x40  fov          (1 float)
offset 0x44  position     (3 floats: x y z)
offset 0x50  rotation     (3 floats: pitch yaw roll, radians)
```

### Step 1 — Find the camera position with Cheat Engine

1. Launch the game, get in-game where you can move the camera.
2. Open **Cheat Engine**, click the computer icon, select the game process
   (the DX11 32-bit one you've been injecting into).
3. Value type **Float**. We'll find your camera's X coordinate by elimination:
   - Move along one axis, then **First Scan** with "Unknown initial value".
   - Move; **Next Scan → Increased value**. Move the other way; **Decreased value**.
   - Repeat until you're down to a handful of addresses. One of them is a camera
     position float. (Position/rotation floats change smoothly as you move/turn —
     that's how you recognise them.)
4. Confirm: change the value in Cheat Engine and watch the camera jump.

### Step 2 — Turn that address into a stable pointer

Raw addresses change every launch (ASLR). You need a **pointer path**:

1. Right-click the found address → **Pointer scan for this address**.
2. Relaunch the game, redo Step 1 to find the new address, then **rescan** the
   pointer-scan results with it. Repeat 2–3 times until you have a short, stable
   path like `"game.exe"+0x01A2B3C4 -> +0x10 -> +0x40`.

The final base + offsets are what you'll hardcode.

### Step 3 — (Optional, for the signature path) get a byte pattern

The mod currently resolves the camera via a **code signature**. If you want to
use that path instead of a hardcoded pointer:

1. In **x32dbg**, attach to the game, right-click the camera address in the dump
   → **Find references** to see the instruction that reads/writes it, e.g.
   `mov ecx, [00A2B3C4]` with bytes `8B 0D C4 B3 A2 00`.
2. Copy ~12–16 bytes starting at that instruction. Replace the 4 address bytes
   with `??` wildcards: `8B 0D ?? ?? ?? ?? ...`.

### Step 4 — Fill it into the code

Open `mod/features/freecam.h`, in the `#else` (32-bit) block:

```cpp
static constexpr const char* CAM_MATRIX_SIG = "8B 0D ?? ?? ?? ?? ...";  // your pattern
static constexpr int CAM_SIG_REL32_OFFSET   = 2;   // offset of the 4-byte address field
                                                   // (2 = right after `8B 0D`)
```

The resolver is already architecture-aware: on a 32-bit build it reads the
operand as an **absolute address** (`ResolvePtrOperand` → `ResolveAbs32`), which
is correct for x86. Rebuild, re-inject, and the console should print
`FreeCam: camera state @ 0x........` instead of "pattern not found".

If the matrix offsets above are wrong for your build (camera moves oddly), adjust
`OFF_VIEW_MATRIX / OFF_FOV / OFF_POSITION` at the top of `mod/features/freecam.cpp`.

---

## Part 2 — Object spawner (advanced / stretch goal)

This needs three things, and is much harder than the camera:

1. **GObjects** — UE3's global object array. Fill `GOBJECTS_SIG` (and the operand
   offset) in `mod/features/object_dumper.cpp`. The Discovery panel in the menu
   shows whether the UE3 path goes active. Without it, the LEAD string-anchor
   fallback needs its node-layout offsets tuned (the `no node references 'Actor'`
   message means those offsets are wrong for your build).
2. **Spawn function** — `UWorld::SpawnActor`. Fill `SPAWN_BY_TYPE_SIG` in
   `mod/features/engine_bridge.h`.
3. **Transform / destroy** — `SET_TRANSFORM_SIG`, `DESTROY_ACTOR_SIG`.

> ⚠️ All of these use the same resolver. On a 32-bit build they must be **x86
> patterns** (no `48` REX prefix) and the operand is an absolute address — the
> code already handles that via `ResolvePtrOperand`. But finding the functions
> and getting their calling convention/arguments right is real RE work; expect
> crashes while iterating (calls are `__try/__except`-guarded, so a wrong guess
> reports an error rather than killing the game).

Realistically: get the **freecam** working first. The spawner is a much larger
undertaking and may not be worth it unless you're comfortable with UE3 internals.
