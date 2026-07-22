# PersistentBuffs tools

## `export_buffs.py` — offline buff-discovery export / validation

Runs the mod's **`build_tracked_speffects()`** logic (src/buff_discovery.cpp +
src/buff_filters.cpp) in Python against any `regulation.bin`, without launching
the game. Use it to:

- **verify coverage** — is the buff you expect actually tracked, and via which
  source (grease / spell / consumable / AoW / utility)? If not, is it dropped as
  a debuff, foe-only, or simply not reachable from a source chain?
- **regenerate / diff the character-wide list** — `--diff-always` compares the
  discovered character-wide AoW self-buffs against the hard-coded
  `g_always_persist` seed in `src/config.hpp`, so an overhaul's new ashes (Oath
  of Vengeance, etc.) that *should* survive any weapon swap are surfaced.
- **cross-check overhauls / sibling mods** — point it at Convergence / Reforged /
  ERR regulation to see what the param-driven discovery picks up there. Shares
  the same param logic as InfiniteWeaponBuffs, so it doubles as a compatibility
  check.

The classification functions (`is_grease`, `is_self_buff`, `is_debuff`,
`is_foe_only`, `is_system_effect`, chain-walk, bullet indirection, enchant-vfx)
are kept **in lock-step with the C++**. If you change a rule in the mod, mirror
it in `export_buffs.py` (each function names its C++ counterpart).

### Setup

```sh
pip install pythonnet      # needs the .NET runtime (coreclr) installed
```

It reuses assets already in the repo — the `Andre.SoulsFormats.dll` vendored in
the sibling `ERR-MapForGoblins-DLL/tools/lib` and the Paramdex ER paramdefs in
`../external/Paramdex/ER/Defs` — so nothing else needs downloading. Point it at a
`regulation.bin` via `--regulation` or `tools/export_buffs.ini` (copy the
`.example`).

### Usage

```sh
# Plumbing self-test (no regulation needed): loads SoulsFormats + paramdefs.
python export_buffs.py --check

# Full report to stdout.
python export_buffs.py --regulation "C:\...\ELDEN RING\Game\regulation.bin"

# Report + JSON dump + diff the character-wide set vs g_always_persist.
python export_buffs.py --regulation reg.bin --json buffs.json --diff-always
```

### Output

A summary line matching the mod's own startup log
(`tracked N SpEffect(s) [greases G, spells S, consumables C (physick tears P),
skills/AoW K, utility U] ...`), the persistable count (tracked minus hard-blocked
system ids), and the character-wide vs weapon-bound split. `--json` writes the
full id sets (with names) for scripting; `--diff-always` prints the two-way diff
against `src/config.hpp`.

> Note: this reproduces the *param-derived* discovery. A handful of runtime-only
> details (the live SpEffect entry payload, weapon-hand ownership) exist only
> in-game and are out of scope here.
