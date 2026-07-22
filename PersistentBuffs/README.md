# PersistentBuffs

A **runtime** native DLL mod for Elden Ring that keeps your active buffs through
**fast travel** and **death** — the two transitions where the engine wipes all
active SpEffects regardless of their duration.

> ✅ **Working:** buffs persist through **fast travel** and **death**
> (confirmed in-game). System/state effects (Roundtable Hold's no-combat block,
> evergaols, …) are **filtered out** so they can't follow you out and lock you
> out of attacks or Torrent — only genuine stat buffs are re-applied. An
> opt-in `[weapon_memory]` feature also keeps **weapon buffs (greases / blade
> spells) through weapon swaps and dual-wielding**.
> Offsets/signatures are game-version specific; verify via the log if your build
> differs.

### What's new in 1.35

- **Wider buff coverage**: buff detection now uses an "accept everything except
  a harmful debuff" filter instead of the old "must show a positive stat" one.
  Self+ally buffs whose timer effect carries no stat of its own — the Golden Vow
  *spell*, Rallying Standard, Blessing of the Erdtree, and similar — are now kept
  through death/fast travel. Effects that would hurt you (attack/defence down,
  reduced max HP/FP/stamina, drains, lowered resistances) are still never
  re-applied, and the engine-state safety blocklist is unchanged.
- **`persist_great_runes` removed**: the Great Rune persistence option was pulled
  because it didn't work as intended in-game. If your `.ini` still has the line,
  it's removed automatically on the next launch. (Physick tear persistence is a
  separate option and is unaffected.)

### What's new in 1.33 (in-game verification pending)

- **Per-weapon memory fixed**: blade spells (Order's Blade, Bloodflame Blade,
  Scholar's Armament, …) no longer follow you onto every weapon you swap to.
  Weapon-bound vs character-wide is now decided from the weapon-enchant VFX in
  the game's params — a signal that survives InfiniteWeaponBuffs'
  `stacking_bonuses` (which zeroes the category field the old check relied on)
  and overhaul regulations. If you saw this bug, **delete
  `PersistentBuffs.state.ini` once** — old files carry the bad bindings.
- **Cleaner log**: the filter no longer prints the `(not-tracked)` /
  `(system)` walls (talismans, armor passives, …) — only drops you configured
  yourself (`never_persist_ids`, physick/great-rune switches) are reported.

### What's new in 1.32 (in-game verification pending)

- **Lantern fix**: the Lantern light no longer stays lost after death — the mod
  now relights it on respawn like any other stripped effect.
- **New option `persist_physick_tears`** (default on): Flask of Wondrous
  Physick tear effects get their own on/off switch instead of hand-listing ids
  in `never_persist_ids`.
- **Simpler config**: `restore_remaining_time` and `restore_buff_power` are
  gone from the `.ini` — both are core behavior now (always on). Buffs always
  come back with their remaining time and their full cast-time power.
- **Self-healing `.ini`**: on every launch the mod fixes its own config —
  missing options are re-added with defaults, removed/unknown options are
  deleted, and your values are kept. Old 1.31 files upgrade automatically.
- **Readable logs built in**: effect and weapon names (Paramdex) are embedded
  in the DLL — the log says `1730:[Weapon] Golden Vow - Damage/Defence Buff`
  out of the box, no loose `SpEffectParam.txt` needed (a file next to the DLL
  still works as an override for overhaul ids). The log itself got a human
  makeover: per-line +/- effect changes, section dividers on death/fast
  travel, readable durations ("4m05s left").

### What's new in 1.31 (in-game verification pending)

- **Re-applied buffs keep their power**: weapon buffs cast from a seal/staff
  (Bloodflame Blade & co.) no longer fall back to flat grease-level damage when
  the mod restores them — the cast-time scaled state is snapshotted and copied
  onto the re-applied buff (`restore_buff_power`, on by default).
- **Incantation buffs (Golden Vow etc.) persist through death again**: buff
  discovery now follows the projectile/bullet path AOE incantations actually
  use, plus all replace/cycle chains. This fixes the 1.30 regression.
- **Death is detected from HP**, not from a buff-drop heuristic: deaths with a
  single active buff are restored too, and `keep_after_death` /
  `keep_after_fast_travel` now work **independently**.
- **All ashes of war** self-buffs are auto-discovered from the game's behavior
  params (no more fixed 32-id list) — overhaul-added skills included.
- **Overhaul-friendly discovery**: items/spells/skills added by overhauls
  (Reforged, Convergence, …) are picked up automatically from params. New
  `never_persist_ids` lets you blacklist anything you don't want restored.
  Pure debuffs are never persisted; status ailments stay hard-blocked.
- **Character switching never leaks buffs** into the newly loaded character,
  whatever features are enabled.
- **Steam Deck / Proton fixed**: the DLL is now fully self-contained (static
  CRT) — it no longer needs the Visual C++ redistributable inside the Proton
  prefix, which is why it previously did nothing on Deck.

> ⚠️ **Offline only.** Memory-editing/hooking mod — run with EasyAntiCheat
> disabled (ModEngine3/2, Elden Mod Loader). Online use risks a ban.

## Why a runtime mod (and not a param edit)?

The companion mod *InfiniteWeaponBuffs* makes buff durations infinite by editing
`SpEffectParam`. But fast travel / death clearing buffs is **hardcoded engine
behavior** — no param controls it (confirmed by research; even the popular
"forever buffs" mods can't do it via params). The only way is at runtime:
remember the player's active buffs and **re-apply** them right after the engine
clears them on a transition.

## Configure

`PersistentBuffs.ini` (next to the DLL, same base name). **The file heals
itself**: anything you delete comes back with its default on the next launch,
anything the mod doesn't know is removed, and your values are kept — so you
can never break it permanently.

```ini
[persistence]
keep_after_fast_travel = 1
keep_after_death       = 1

; Flask of Wondrous Physick tear effects: keep them through death/fast travel
; like any other buff. Set to 0 if you consider that cheating.
persist_physick_tears = 1

; Only genuine stat buffs are persisted; system/state effects are auto-filtered.
; force_persist_ids = ids to ALWAYS persist (rescue a real buff the filter drops
; by mistake). Comma-separated; find the id in the log with log_effects = 1.
force_persist_ids =
; never_persist_ids = ids to NEVER persist (e.g. a misbehaving overhaul
; effect). Beats every other rule.
never_persist_ids =

[weapon_memory]
; Remembers greases/blade buffs per weapon and restores them after a loadout
; change (swapping weapons, or bringing a left-hand weapon into play /
; dual-wielding, which vanilla drops the buff on). Body buffs (Golden Vow,
; consumables) are left alone.
remember_per_weapon = 1
```

Buffs are **always** restored with their remaining time and their cast-time
power (seal/staff scaling — the +75-instead-of-+200 fix); the old
`restore_remaining_time` / `restore_buff_power` switches are gone. The AoW
self-buff list (Endure, Determination, Roars, …) and the re-apply delay are
**hard-coded** (no longer `.ini` options).

**Readable logs:** effect and weapon names are built into the DLL (Paramdex),
so the log prints `id:Name` out of the box. A `SpEffectParam.txt` next to the
DLL (same Paramdex format) overrides/extends the built-in names — useful for
overhaul mods with custom effects.

## Build

Prerequisites: Visual Studio 2022 (Desktop C++), CMake ≥ 3.15, Git.

```sh
git submodule update --init --recursive   # pulls MinHook + libER + Paramdex
```

Then run `build.bat`, or:

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/PersistentBuffs.dll` (links MinHook + libER statically — no
extra DLLs to ship). libER is used only to read `SpEffectParam` for the buff
filter; the Paramdex submodule feeds the embedded name lists at build time.

## Logs

Every launch writes `logs/PersistentBuffs.log` next to the DLL — including the
list of active SpEffects it sees, which is how you verify the offsets are right
for your game version.

## Status / internals

See **[CLAUDE.md](CLAUDE.md)** for the architecture, the exact offsets/signatures
used, what's verified vs. what still needs RE, and the step-by-step plan to
finish it.
