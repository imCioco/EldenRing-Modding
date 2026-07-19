# Adjustable Spell Cost

A native C++ DLL mod for Elden Ring that makes **sorceries and incantations** cheaper to cast and easier to meet the stat requirements for. Three independent knobs, set in the `.ini`:

- **Divide the FP cost** of every spell by a number you choose, with a minimum-cost floor.
- **Divide the stat requirements** (Intelligence / Faith / Arcane) of every spell by a number you choose, floored at 1.
- **Divide the stat requirements needed to wield every staff and sacred seal** (Strength / Dex / Intelligence / Faith / Arcane) by a number you choose, floored at 1.

It rewrites param fields in memory at runtime (via [libER](https://github.com/Dasaav-dsv/libER)) — **no `regulation.bin` edit**:

| Field | Meaning | Action |
|---|---|---|
| `Magic.mp` | FP cost (normal cast) | divided by `[fp_cost] divisor`, floored |
| `Magic.mp_charge` | FP cost (charged cast) | divided by `[fp_cost] divisor`, floored |
| `Magic.requirementIntellect` | Intelligence requirement | divided by `[stat_requirements] divisor`, min 1 |
| `Magic.requirementFaith` | Faith requirement | divided by `[stat_requirements] divisor`, min 1 |
| `Magic.requirementLuck` | Arcane requirement (Arcane = "Luck" internally) | divided by `[stat_requirements] divisor`, min 1 |
| `EquipParamWeapon.properStrength/Agility/Magic/Faith/Luck` | Catalyst (staff/seal) stat requirements | divided by `[catalyst_stat_requirements] divisor`, min 1 |

Catalysts are identified the same way [OmniCaster](../OmniCaster) does it: a weapon row in `EquipParamWeapon` with `enableMagic` (staff) and/or `enableMiracle` (seal) set. Rows with neither are normal weapons and are left untouched. (Deliberately "at least one", not "exactly one" — if OmniCaster's `cast_anything` is also loaded, it flips both flags on every catalyst, and there's no load-order guarantee between the two mods' independent worker threads.)

> ⚠️ **Offline only.** Elden Ring runs EasyAntiCheat. Run with a mod loader that disables it (ModEngine3/me3, ModEngine2, or Elden Mod Loader). Single-player / Seamless Co-op only.

---

## How the two limits work

**FP cost floor (`[fp_cost] min_cost`).** After dividing, a cost is never dropped below `min_cost`. But if a spell's **vanilla** cost is already below the floor, that cheaper vanilla cost is kept — the floor only ever stops a cost going *lower*, it never raises one. Example with `divisor = 3`, `min_cost = 5`:

- Rock Sling (18 FP) → `18/3 = 6` → **6 FP**.
- A 12-FP spell → `12/3 = 4`, below the floor → **5 FP**.
- A 3-FP spell (already under the floor) → left at **3 FP**.

**Requirement floor (fixed at 1).** Requirements are divided the same way but the minimum is hard-wired to 1 — there's no `.ini` knob for it. A spell that requires a stat will always require at least 1 point of it, and a stat the spell never needed (requirement `0`, e.g. Intelligence on a pure incantation) stays `0` so the mod never invents a new requirement.

## Why there's no "is-spell" filter

In Elden Ring the `Magic` param contains **only** sorceries and incantations — nothing else lives there (Ashes of War are in a separate param, greases/consumables are `EquipParamGoods`). So, unlike the goods-based mods in this repo that filter by `sortGroupId`, this mod simply iterates every `Magic` row; a row is skipped only if it has no cost and no requirement at all (empty/unused rows). This is the same "iterate `from::param::Magic`" approach `InfiniteWeaponBuffs` uses for spell buffs.

---

## Configure

Edit `AdjustableSpellCost.ini` (next to the DLL, same base name):

```ini
[fp_cost]
divisor  = 2     ; FP cost ÷ 2 (half). 1 = no change, decimals allowed.
min_cost = 5     ; floor; a spell already cheaper than this keeps its vanilla cost.

[stat_requirements]
divisor  = 2     ; INT/Faith/Arcane requirements ÷ 2. 1 = no change. Min is always 1.

[catalyst_stat_requirements]
divisor  = 2     ; staff/seal STR/DEX/INT/Faith/Arcane requirements ÷ 2. 1 = no change. Min is always 1.
```

Set any `divisor` to `1` to leave that aspect untouched (e.g. lower FP cost only, keep vanilla requirements). `[debugging] log_each = 1` writes every spell's and catalyst's before/after values to the log — handy for verifying against the in-game menus.

---

## Build

**Prerequisites:** Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.15, Git.

```sh
git submodule add https://github.com/Dasaav-dsv/libER external/libER
git submodule update --init --recursive

cmake -B build -A x64
cmake --build build --config Release
```

Or just run **`build.bat`** (configures on first run, then builds Release).

Output: `build/Release/AdjustableSpellCost.dll` — a single self-contained DLL (libER is linked statically, so there's no `libER.dll` to ship).

## Install

Copy `AdjustableSpellCost.dll` + `AdjustableSpellCost.ini` into your mod loader's native-mod folder (e.g. the me3 profile folder), launch offline.

## Logs

Every launch writes `logs/AdjustableSpellCost.log` next to the DLL. A success run ends with, e.g.:

```
fp_divisor=2.00 (min 5) req_divisor=2.00: 318 spells -> FP changed on 300, requirements changed on 312
catalyst_requirements_divisor=2.00: 61 staffs/seals -> requirements changed on 61
```

`0 spells` would mean the `Magic` param didn't resolve — usually a libER/game-version mismatch (see the note in the other mods' READMEs). Flip `log_each = 1` to inspect individual spells.
