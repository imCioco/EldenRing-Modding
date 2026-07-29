# Lantern Torch

Turns the Lantern into a torch. Pick a torch in the `.ini`, and the effect the
lantern applies while it's equipped is replaced — in memory — by that torch's
passive effect. Hands stay free, and the lantern now does what the torch does.

No `regulation.bin` edit, no FXR patching, no game files touched.

## Install

Drop `LanternTorch.dll` and `LanternTorch.ini` into your mod loader's `mods`
folder (Elden Mod Loader / ModEngine2), next to each other. **Offline only** —
EasyAntiCheat must be disabled.

A log is written to `logs\LanternTorch.log` next to the DLL every launch.

## Configuring

The only setting that matters is which torch to copy:

```ini
[lantern]
torch = 24070000    ; Sentry's Torch
```

| ID | Torch | Passive trait |
|----|-------|---------------|
| `24000000` | Torch | fire aura (SpEffect 415) |
| `24020000` | Steel-Wire Torch | fire aura — its bleed is on attacks |
| `24040000` | St. Trina's Torch | fire aura — its sleep is on attacks |
| `24050000` | Ghostflame Torch | fire aura — its frost is on attacks |
| `24060000` | Beast-Repellent Torch | fire aura **+ drives beasts away** (460) |
| `24070000` | Sentry's Torch | fire aura **+ reveals invisible enemies** (416) |
| `24500000` | Nanaya's Torch | fire aura |
| `16080000` | Torchpole | nothing — it has no resident effect |
| `0` | — | no effect applied |

### What can and can't be mirrored

Only a torch's **resident** effect — the one it applies just for being in your
hand — can move to the lantern. Traits that live on the torch's *attack data*
(St. Trina's sleep, Steel-Wire's bleed, Ghostflame's frost) fire when you swing
it, and a lantern never swings, so there's nothing there to copy.

Every torch shares the same fire aura, SpEffect 415. Beast-Repellent and
Sentry's are the only two carrying a second effect of their own on top of it —
they're the interesting picks. Torchpole has no resident effect at all; pick it
and the mod changes nothing and says so in the log.

### The light

The lantern keeps its own light — a torch's flame can't be copied, because it
hangs off the weapon model and no param carries it (every torch's effect row
reads `vfx = -1`). What the mod does instead is make the lantern's light
*stronger*:

```ini
[light]
radius    = 24     ; how far it lights up around you (vanilla 16)
intensity = 2.0    ; how strong that light is    (vanilla 1.25)
```

Either can be set to `0` to leave it at its vanilla value.

These live inside the light's effect file (`f000302421.fxr`) rather than in a
param, so they're written into the copy the game has loaded in memory. The
effect isn't loaded at startup, so the mod keeps looking for it for up to two
minutes after the game boots and writes as soon as it appears — the log records
the old and new values.

The signature and offsets for that come from
[CustomLantern](https://github.com/0-F/CustomLantern) by 0-F, which is the mod
to use if you want the full set of light controls — colour, position, specular.
Nothing here conflicts with it; both write to the same loaded effect, so pick
one to own the light if you run both.

### Finding out what's actually in your params

```ini
[debugging]
dump = 1
```

Changes nothing and writes the lantern's rows and every torch's rows to the log.

## How it works

| Step | Param |
|------|-------|
| Find the lantern's effect rows | `EquipParamGoods[2070]` → its ref ids (fallback: `SpEffectParam` 3245 / 3246, "[Item] Lantern - Right/Left") |
| Find the torch's passive effect | `EquipParamWeapon[<torch>].residentSpEffectId{,1,2}` |
| Mirror | the whole torch `SpEffectParam` row is copied onto the lantern's row |
| Put the lantern back on top | `effectEndurance`, `motionInterval`, `stateInfo`, `iconId`, `vfxId0..7` are restored from the lantern row |
| The light | radius / intensity floats written into the loaded `f000302421.fxr` |

Those restored fields are what keep the mirrored effect behaving like a *lantern*
effect: the right lifetime, the engine's own handle on it, and the HUD icon. So
the only two things that actually change are the passive effect and the light.

The lantern has a row per hand, but only one is real — the vanilla Left row
(3246) is an empty stub with no duration, so anything written there would never
fire. The effect goes on the live row, and since all torches share the generic
fire aura, the torch's *last* resident effect (its distinctive one) is the one
applied.

## Building

Requires CMake and MSVC (x64).

```bash
git submodule update --init --recursive
```

Then run `build.bat`, or:

```bash
cmake -S . -B build -A x64 && cmake --build build --config Release
```

Output: `build\Release\LanternTorch.dll`.

Built on [libER](https://github.com/Dasaav-dsv/libER). Param row ids and names
come from [Paramdex](https://github.com/soulsmods/Paramdex).
