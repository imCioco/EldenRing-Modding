# OmniCaster

Cast **any spell with any catalyst** in ELDEN RING — staffs cast incantations,
seals cast sorceries — and make spell damage **scale with whatever you're
holding** (or, optionally, with your highest casting stat).

A native DLL that patches the game's param tables in memory after they load.
No `regulation.bin` edits, no files replaced — remove the DLL and everything
is vanilla again. **Offline use only** (EasyAntiCheat must be disabled; the
usual mod loaders handle that).

## Features

- **Cross-casting** (`cast_anything = 1`): every staff and seal can cast both
  sorceries and incantations.
- **Unified scaling** (`scaling_mode = equipped`, the default): a spell's
  power comes from the catalyst in your hand. Incantations cast with a staff
  use that staff's sorcery scaling; sorceries cast with a seal use that
  seal's incant scaling. Upgrade levels count (a +25 staff empowers incants
  just as much as sorceries).
- **Highest-stat mode** (`scaling_mode = highest_stat`): every catalyst
  scales *both* spell types off the higher of your INT and FAI. Uses your
  **effective** stats — talismans, the physick, and buffs that raise INT/FAI
  count. It follows the currently loaded character rather than the
  character-select/menu copy. Tracked live: load another character, level up,
  respec, or swap a stat talisman and the scaling stat flips within ~1 second.
- **Highest-stat requirements** (`requirement_swap = 1`, off by default): a
  spell's INT/FAI requirement is checked against whichever of the two is
  higher for you. The number never changes — Meteorite still asks for 30, a
  faith character just meets it with 30 FAI instead of 30 INT, so it casts
  instead of fizzling. Spells needing both stats collapse onto the larger of
  the two requirements; Arcane requirements are left alone. Tracked live on
  the same ~1 second poll, and independent of `scaling_mode` (you can enable
  either one without the other).

## What is deliberately NOT changed

- **Catalyst wield requirements**: staffs and seals keep their own stat
  requirements, so a pure-faith build still takes the wield penalty holding a
  staff. Only *spell* requirements are remapped, and only with
  `requirement_swap = 1`.
- **School boost passives** stay type-bound: Gravel Stone Seal still boosts
  only dragon incants, the Academy Glintstone Staff only full-moon
  sorceries, etc.
- **Hybrid catalysts** (Prince of Death's Staff, Golden Order Seal, …) work
  naturally in `equipped` mode — their dual-stat scaling is mirrored as-is.
  In `highest_stat` mode they become single-stat like everything else
  (that's what "highest stat" means); secondary non-casting stats such as
  the Dragon Communion Seal's arcane keep working.

## Install

1. Copy `release\OmniCaster.dll` and `release\OmniCaster.ini` into your DLL
   mod loader's mods directory (me3 / ModEngine2 / elden_mod_loader).
2. Edit `OmniCaster.ini` if you want a different scaling mode.
3. Play offline.

A log is written to `logs\OmniCaster.log` next to the DLL. Set
`[debugging] dump = 1` to log every catalyst that gets patched, by name.

## Build

Requirements: Visual Studio 2022 (Desktop C++), CMake 3.15+.

```
git submodule update --init --recursive
build.bat
```

Output lands in `release\OmniCaster.dll`. [libER](https://github.com/Dasaav-dsv/libER)
is statically linked; weapon names from
[Paramdex](https://github.com/soulsmods/Paramdex) are embedded at build time.

## How it works

Spell power in ELDEN RING is the catalyst's hidden elemental attack rating:
sorceries multiply by the weapon's **magic** attack power, incantations by
its **holy** attack power. OmniCaster mirrors each catalyst's real side onto
the other across the three tables that produce that rating:

| Param table | What's mirrored |
|---|---|
| `EquipParamWeapon` | `attackBaseMagic` ↔ `attackBaseDark`, scaling curve ids, plus the `enableMagic`/`enableMiracle` cast-permission flags |
| `AttackElementCorrectParam` | which stats (INT/FAI/…) feed each element, with their influence rates |
| `ReinforceParamWeapon` | per-upgrade-level growth of the mirrored element |

Correction rows shared with non-catalyst weapons are detected and skipped
(logged) so melee scaling can't be corrupted. `highest_stat` mode
additionally reads your **effective** INT/FAI (the exact numbers the status
screen shows, gear and buffs included) from the current in-world `PlayerIns`.
The mod reads that character's base stats and the INT/FAI corrections on its
active game effects, then polls the result once a second. This is necessary
because `PlayerGameData`'s stored “effective” block does not include every
live correction (Godrick's Great Rune is one observed example).

`requirement_swap` uses that same poll and a fourth table, `Magic`: each
spell's `requirementIntellect`/`requirementFaith` pair is snapshotted at
startup, and every rewrite comes from that snapshot — so the requirement is
never compounded, only moved.

**Compatibility with [AdjustableSpellCost](../AdjustableSpellCost)**: both mods
write those same two fields from their own threads, with no way to order each
other. OmniCaster therefore remembers exactly what it wrote and re-checks the
rows once a second: anything it finds there that it didn't put there (i.e.
AdjustableSpellCost's `stat_requirements` divider) is adopted as the new
baseline, and the swap is re-applied on top of it. Whichever mod's pass runs
first, within a second you get the **divided** requirement, sitting on your
**higher** casting stat. Hybrid INT+FAI spells collapse to the larger of the
two before division, which is the same number either way. AdjustableSpellCost's
FP costs and catalyst requirements are untouched by OmniCaster, so those always
apply. The adoption is logged once per event (`adopted an outside edit on N
spell row(s)`).

## Overhaul mods (Convergence, Reforged, …)

Should broadly work: nothing is hardcoded to vanilla weapon ids. Catalysts
are found by their cast-permission flags, so an overhaul's new staffs/seals
are picked up automatically (they show as `#<id>` in the log — names come
from the vanilla list). Catalysts that natively cast both spell types (like
the vanilla Staff of the Great Beyond, or overhaul hybrids) are deliberately
left untouched — their scaling is the overhaul designer's. Two caveats:

- If the overhaul ships its own DLL that rewrites the same catalyst params at
  runtime, last-writer wins; OmniCaster applies once at startup and doesn't
  fight back (except `highest_stat`'s stat flips).
- Correction rows an overhaul shares between catalysts and melee weapons are
  skipped for safety (check the log for `shared with non-catalyst` warnings —
  those weapons keep their overhaul scaling for cross-cast spells).

## Known caveats

- **Menu display**: the equipment screen shows a single "Sorcery Scaling"
  line on every catalyst (the UI picks one label; it can't be changed via
  params). Cosmetic only — the number shown applies to both spell types,
  since both sides are mirrored to the same value.
- `scaling_mode = off` + `cast_anything = 1` lets you cast cross-type spells,
  but they'll hit like wet noodles (the catalyst has ~0 attack rating for the
  foreign element). Use `equipped` unless you know what you're doing.
- Casting animations come from the catalyst you're holding (staff animations
  for incants, chime animations for sorceries).
- The Carian Sorcery Sword shares its stat-correction row with regular melee
  weapons, so it's skipped there (see the log): incantations cast with it get
  its flat holy rating but won't gain INT scaling. Every dedicated staff/seal
  is unaffected.
- In-memory only: must be re-applied by the loader every launch (automatic),
  and incompatible with anything that expects vanilla catalyst params.
