# ER Infinite Weapon Buffs

A native C++ DLL mod for Elden Ring that:

1. **Makes every weapon buffable** — any grease or spell buff can be applied to any weapon.
2. **Sets buff durations** — greases, spell buffs and consumable buffs last as long as you configure (including permanent), per category, via `InfiniteWeaponBuffs.ini`.
3. **Mirrors weapon buffs to both hands** (optional) — when dual-wielding, a grease or weapon-skill enchant applied to your main hand also lands on the off-hand weapon.

It does this entirely by **rewriting param tables in memory at runtime** (via [libER](https://github.com/Dasaav-dsv/libER)). **No `regulation.bin` edit is involved.**

> ⚠️ **Offline only.** Elden Ring runs EasyAntiCheat. Any memory-editing mod must run with EAC disabled (the mod loaders below handle that). Using this online risks a soft-ban. Single-player or Seamless Co-op only.

---

## How it works

| Feature | What it changes in memory |
|---|---|
| All weapons buffable | `EquipParamWeapon.isEnhance = true` on every row |
| Buff durations | `SpEffectParam.effectEndurance` on the IDs you list (`-1` = permanent, else seconds) |
| Dual-wield mirror (opt-in) | `SpEffectParam.cycleOccurrenceSpEffectId` on each buff's Right (main-hand) effect → its Left (off-hand) effect |

Edits are applied once, on a worker thread, right after `SoloParamRepository::wait_for_params()` reports the param tables are loaded. They persist for the rest of the session.

### What "infinite" really means
With infinite duration, buffs **persist through resting at a grace and fast travel**. The engine still clears active SpEffects on **death** regardless of duration, so in practice `infinite` lasts until you die — then re-apply.

---

## Build

**Prerequisites:** Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.15, Git.

```sh
# 1. Get the code + libER
git init
git submodule add https://github.com/Dasaav-dsv/libER external/libER
git submodule update --init --recursive

# 2. Configure + build (Release)
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/InfiniteWeaponBuffs.dll`

---

## Install

Works with any native-DLL loader — **ModEngine3 (me3)**, ModEngine2, or Elden Mod Loader. Run offline.

- **me3:** add `InfiniteWeaponBuffs.dll` as a native mod in your profile (the same folder as your other native mods, e.g. `...\me3\config\profiles\<profile>\`). Put `InfiniteWeaponBuffs.ini` right next to the DLL.
- **ModEngine2:** add the DLL to `external_dlls` in the TOML, or drop it in a folder Elden Mod Loader scans.
- **Elden Mod Loader:** drop the DLL + `.ini` in `mods/`.

The `.ini` **must** sit next to the DLL and share its base name (`InfiniteWeaponBuffs.dll` → `InfiniteWeaponBuffs.ini`).

---

## Configure

Edit `InfiniteWeaponBuffs.ini` (kept next to the DLL, sharing its base name). It's organized as `[general]`, one section per buff category, then `[stacking]`, `[dual_wield]` and `[discover]`:

```ini
[general]
all_weapons_buffable = 1
extra_goods =            ; optional: extra goods ids to treat as buff consumables

[greases]
enabled  = 1
duration = 300           ; seconds, or "infinite" (= permanent)
[spell_buffs]
duration = infinite
[consumables]
duration = infinite
[ashes_of_war]
duration = infinite
speffect_ids =           ; optional: Ash-of-War buff SpEffect ids (see dump)
```

`extra_goods` / `speffect_ids` are precise id allowlists (not category numbers), **unioned with** the built-in defaults — leave them empty unless you need to add something the heuristics miss.

### How categories are derived

Categorization is code-driven from live param fields (no `goodsType` lists to maintain), so it stays correct across game patches and alongside other mods:

- **Greases** = goods that enchant a weapon/shield (`EquipParamGoods.isEnhance` / `isShieldEnchant`) or sit in sort group `70`.
- **Consumables** = buff/heal goods (`sortGroupId == 20`: Exalted Flesh, cured meats, livers, boluses…) plus an explicit `extra_goods` allowlist. The default allowlist already includes the DLC **Golden Vow** pot (`2003170`), which delivers its buff through a thrown bullet rather than a plain refId.
- **Spell buffs** = SpEffects referenced by the `Magic` param. A spell reaches its buff either **directly** (self-only buffs like Flame Grant Me Strength put the SpEffect straight in a refId) or **through an area-effect bullet** (self+ally buffs like Golden Vow, Barrier of Gold, Rallying Standard and Blessing of the Erdtree apply the buff via an AoE that hits you and your allies). Both paths are followed — the second one is the fix for the long-standing "buffs that also help allies don't last" reports.
- **Ashes of War** = a curated allowlist of weapon-skill buff SpEffect ids (built-in defaults + `[ashes_of_war] speffect_ids`). An activated skill's buff is applied through a behavior that can't be reached from the gem param (`SwordArtsParam` has no SpEffect/bullet ref), so a curated id list from soulsmods/Paramdex drives it. The defaults now aim to cover **every buff-granting Ash of War** — Roar, Endure, Determination, Royal Knight's Resolve, Golden Vow, War Cry, Braggart's Roar, Jellyfish Shield, **Oath of Vengeance**, Sacred/Shared Order, Last Rites, Barricade Shield, Assassin's Gambit, and the element weapon-enchant arts (Cragblade, Flaming Strike, Sacred Blade, Chilling/Poison Mist, Moonlight Greatsword, Ice Lightning Sword, …). Add any id via `speffect_ids` if a mod changes them or `dump=1` reports one MISSING.

Allowlisted ids are applied directly (the curated list is trusted, so the debuff/target filter is skipped — an ash's timer row often carries no stat field of its own); only the id's own finite timer is required. **Engine system effects are always excluded** by an id blocklist (e.g. `9621` Roundtable "no-combat", animation/grace states, `4600` Wet), independent of struct fields, so they can never be made permanent.

For each consumable and spell the buff SpEffect is found by following the item's refId (as a SpEffect, or — for thrown pots / AoE spells — as a `Bullet`), its `behavior → bullet` chain, and the SpEffect `replace`/`cycle` chain, then keeping only effects that are **on a finite timer**, **not a debuff**, and **not enemy-only-targeted** — so instant heals, passives, already-permanent buffs, offensive spell payloads, debuffs and system/state effects are all left alone.

**Debuffs stay untouched; everything else is extended.** Rather than a narrow "must be a positive buff" whitelist (which dropped ally-and-self buffs whose timer row carries no stat field), consumables and spell buffs use a **debuff blacklist**: an effect is skipped only if it *harms the bearer* — lowers your max/current HP·FP·stamina, drains them over time, weakens attack or defense, raises damage taken, or lowers status resistance — or if it targets **enemies only**. Self buffs, ally buffs and neutral/utility effects all pass. Engine states are still stopped by the `is_system_effect` id blocklist (e.g. the Roundtable "no-combat" state) and the horse-protected set. Greases and ashes of war come from trusted, narrow sources and skip this filter entirely.

**Torrent is protected:** every SpEffect reachable from a horse-summon item (`isSummonHorse`, plus the built-in Spectral Steed Whistle id `130`) is fenced off and never patched, so Torrent's "active" state can't get stuck.

> **Scope note:** "spell buffs" and "consumables" are broad — they cover *all* timed self buffs those sources apply, not only weapon-enchant spells / stat-buff foods.

### Experimental: stacking

One optional extra (**default off**). Only touches the buffs this mod already manages, and is a plain param edit — so **test in-game**.

```ini
[stacking]
stacking_bonuses = 0     ; let buffs coexist instead of replacing each other
```

- **`stacking_bonuses`** zeroes `SpEffectParam.spCategory`, removing the mutual-exclusion that makes some buffs replace one another — so you can stack buffs that normally can't coexist.

### Experimental: dual-wield off-hand mirror

One optional extra (**default off**). When you're dual-wielding and apply a buff to your main hand, this also enchants the **off-hand** weapon. It's a plain param edit — **test in-game**.

```ini
[dual_wield]
mirror_to_offhand = 0    ; apply weapon buffs to BOTH hands when dual-wielding
extra_pairs =            ; optional extra right:left SpEffect id pairs (e.g. mods)
```

**How it works.** Every weapon buff exists as two `SpEffectParam` rows — a *Right* (`wepParamChange=1`, main hand) and a *Left* (`wepParamChange=2`, off hand). The engine normally applies only the main-hand one. With `mirror_to_offhand = 1`, each buff's Right effect gets `cycleOccurrenceSpEffectId` pointed at its Left effect, so while the main-hand buff is active the engine re-applies the off-hand enchant every cycle — the off-hand weapon gets the full enchant (element + visual + on-hit status proc). Direction is **main-hand → off-hand** only (the hand the game buffs by default); the off-hand inherits the main hand's configured duration.

**What's covered:**

- **Greases** (all vanilla + DLC) — discovered the same way as the duration feature (`is_grease`), so the set stays correct across patches and even covers greases added by other mods. The Right/Left partner is matched by the shared weapon-enchant id (`SpEffectVfxParam.soulParamIdForWepEnchant`), with the variant disambiguated by duration (so e.g. full Fire Grease pairs with full, Drawstring with Drawstring).
- **Weapon-skill (Ash of War) enchants** — Cragblade, Chilling/Poison Mist, Seppuku, Ice Lightning Sword, Flame Skewer/Spear, Moonlight Greatsword, Ruinous Ghostflame, Sacred Blade, Lightning Slash, Flaming Strike. These aren't items, so they use a built-in id pair list; add more with `extra_pairs` (format `right:left, right:left`).

**Not covered — pure spell enchants.** Scholar's Armament, Black Flame Blade, Bloodflame Blade, Order's Blade, Electrify Armament and Poison Armament have **no off-hand effect row in vanilla**. Mirroring them would require *creating* new param rows, which this mod can't do (it edits params in memory at runtime; libER has no way to add rows to a param table). They stay on the main hand only. *(The reference regulation.bin mod that inspired this added those rows offline — not possible without a `regulation.bin` edit.)*

**Duration note.** The off-hand is refreshed by the main hand's cycle, so with finite durations the off-hand can linger up to its own duration after the main-hand buff ends. With the mod's usual `infinite` durations both hands match exactly — recommended.

> **Tip:** turn on `[discover] dump = 1` together with this and check the **`DUAL-WIELD PAIRS`** section in the log first — it lists every Right→Left pair that would be wired (and flags any main-hand effect whose `cycleOccurrenceSpEffectId` isn't empty), so you can verify coverage before committing.

### Diagnostic dump

Set `dump = 1` under `[discover]` to log how each category resolves (greases, consumables, spell buffs), which SpEffects are protected as horse-summons, a **potential-misses** list of uncategorized goods that still reach a self timed buff, and an **ash-of-war allowlist check** (which of your `[ashes_of_war]` ids exist and will be extended on this game version) — and change nothing. `debug_console = 1` (also under `[discover]`) mirrors the log to a console window.

---

## Logs / troubleshooting

Every launch writes **`logs/InfiniteWeaponBuffs.log`** (a `logs/` subfolder next to the DLL, created automatically; always on, no config needed). It's the fastest way to see what happened:

- **No `.log` file at all** → the DLL never loaded. Check the loader actually lists it, and that it's not failing with a missing dependency (error 126).
- Log stops after `waiting for params...` → `wait_for_params` never returned. Usually a **libER symbol/version mismatch** (see below).
- `isEnhance set on N weapon rows` and `patched N ... SpEffectParam rows` → it worked; check in-game.

### ⚠️ libER symbol/version caveat
libER resolves game functions from symbol tables baked in at compile time; libER's repo was last updated **2025-04-24**. If your game build (e.g. **1.16.2.0**) is newer than libER's symbols, the param calls can resolve to the wrong addresses — expect a hang at `waiting for params...`, a crash, or `0 rows` patched. If that happens, you need libER symbols updated for your game version (or pin the game to a version libER supports).

---

## Notes / roadmap

- Targets a specific game build; FromSoft patches can shift internal signatures. libER abstracts most of that, but pin a tested game version.
- Possible future enhancement: auto-discover category IDs by scanning `EquipParamGoods` / `Magic` for buff references, instead of hand-listing them.
