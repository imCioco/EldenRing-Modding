# Custom Talisman Effects

A native C++ DLL mod for Elden Ring that lets you turn on **any talisman's effects as a passive buff — without equipping the talisman**. No talisman slot is used, no inventory item is needed; you just flip the talisman to `1` in the `.ini` and its effects are applied to your character as if it were worn.

How it works: in Elden Ring the `EquipParamAccessory` param **is** the talisman table, and each row carries `residentSpEffectId1..4` — the special effects the game applies while that talisman is equipped. This mod reads those SpEffect ids for every enabled talisman and keeps them present on the player via the game's own "apply SpEffect" function. A talisman that only acts *in certain circumstances* (e.g. below a HP threshold) behaves identically here, because that condition logic lives **inside** the SpEffect, not in the act of equipping.

> ⚠️ **Offline only.** Elden Ring runs EasyAntiCheat. Run with a mod loader that disables it (ModEngine3/me3, ModEngine2, or Elden Mod Loader). Single-player / Seamless Co-op only.

---

## In-game overlay

Press **Insert** (keyboard) or **L3 + R3** (controller) to open a Dear ImGui panel — drawn into the game's own screen — where you can toggle any talisman's effect on/off **live**, with the mouse or gamepad. Checking a box applies the effect instantly; unchecking removes it instantly. Close with **Insert**/**L3+R3** again, or **Esc** / **B**. Your input is frozen while the panel is open, so the character won't move. Both are configurable in the `.ini` (`[overlay]` `toggle_key`, `toggle_gamepad_combo`).

**Talisman families & stacking.** In vanilla a talisman's base/+1/+2/+3 versions can't be worn together — they share an `accessoryGroup`. The mod enforces the same rule: enabling one automatically disables the others in its family. Tick **Allow stacking (ignore talisman families)** to bypass this and enable any combination at once.

> Note on stacking: the game may refuse to *add* the numbers of effects that share an internal stack category (many upgrade tiers do) — "Allow stacking" lets you enable them, but whether the bonus actually stacks is up to the engine.

**Already-equipped talismans.** Talismans you have physically equipped are detected (by their effect being live on your character) and shown in **blue**. With stacking **off** they're also greyed out and locked — their effect is already active, so there's nothing to add. With stacking **on** they stay blue but remain toggleable. Unequip the talisman in-game and the row unlocks within about half a second.

Changes made in the overlay are written back to `CustomTalismanEffects.ini` (on **Save to .ini** or when you close the panel), so they persist across sessions — including where you dragged the panel and how big you made it.

The panel scales itself to your resolution (1440p and above get proportionally larger text and controls, 1080p keeps the authored size), so it stays readable on 4K displays. Force a specific size with `[overlay] ui_scale` if you prefer.

**Effect descriptions on hover.** When you point at a talisman with the mouse (or land on it with the controller's stick/d-pad), its effect is shown — as a mouse tooltip and in a detail pane at the bottom of the panel. The descriptions are **baked into the DLL** (no extra file to ship), stored in `src/talisman_names.hpp` as the third field of each entry:

```cpp
{ 1150, "Green Turtle Talisman", "Raises stamina recovery speed" },
{ 8220, "Rellana's Cameo",       "" },
```

Base-game talismans are pre-filled with the game's own effect wording. The 39 Shadow of the Erdtree talismans ship **blank** — to fill them (e.g. from [Fextralife](https://eldenring.wiki.fextralife.com/Talismans), ideally with the % numbers), just edit the `effect` string in `src/talisman_names.hpp` and rebuild. Re-running `python tools/gen_talisman_data.py` **preserves your hand-edited effect text** (only ids/names are regenerated from Paramdex); don't change the `id` or `name`.

The overlay composites itself into the game's own D3D12 backbuffer just before the real `Present`, using a private command list, descriptor heap and fence — the game keeps owning its window, device, command queue, swapchain and `Present` call, and the mod never creates a second one of any of them. Discovery happens by observing `CreateSwapChain`/`CreateSwapChainForHwnd` (hooked early, on the worker thread, before anything slow) and shadowing that swapchain's vtable per instance, so it chains cleanly with other injected overlays, frame-gen mods and Special K instead of fighting them. There is no hard-coded address to maintain. Input is captured focus-free while the panel is open (raw input re-targeted at a message-only window, pad APIs detoured to hand the game a neutral controller), so the game never loses activation.

---

## Use

Edit `CustomTalismanEffects.ini` (next to the DLL). The `[talismans]` section lists every talisman in the game — base versions, every upgrade (`+1`/`+2`/`+3`), and Shadow of the Erdtree DLC talismans — one per line. Set a talisman to `1` to enable it:

```ini
[overlay]
toggle_key           = Insert   ; key to open/close the in-game panel
toggle_gamepad_combo = L3+R3    ; controller combo (buttons joined with +)
;   Available buttons: A B X Y  LB RB  L3 R3  Start Back  Up Down Left Right
allow_stacking       = 0        ; 1 = ignore talisman families (stack anything)
ui_scale             = 0.00     ; 0 = auto-scale with your resolution; or force 0.50 - 2.00

[panel]
; The panel's size & position, remembered automatically when you close it.
; Delete this section to reset the panel to its default placement.

[talismans]
Erdtree's Favor +2           = 1
Green Turtle Talisman        = 1
Radagon's Soreseal           = 0
Crimson Amber Medallion      = 0
; ...155 talismans...

[debugging]
log_each      = 1   ; log each SpEffect applied / removed
debug_console = 0   ; 1 opens a console window with live logs
```

Notes:

- **One line per talisman row.** The base talisman, each upgrade, and each DLC talisman are separate entries — enable exactly the version you want (the `+2` version applies the stronger `+2` effect).
- **Names come from Paramdex** — don't rename them. Matching is case-insensitive and whitespace-tolerant, but the name must otherwise match a line in the default `.ini`. An unknown name is logged and skipped.
- **DLC talismans** are listed; if you don't own the DLC those rows aren't in your game and are skipped harmlessly.
- **No double-up.** If you both enable a talisman here *and* physically equip it, the effect is only applied once (the mod skips ids already active on you).
- Effects persist through fast travel and death — the mod re-asserts them within ~½ second of returning to gameplay.

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

The first configure fetches **Dear ImGui** (v1.90.9) and **MinHook** via CMake `FetchContent` (needs network). libER, ImGui and MinHook are all linked statically.

Output: `build/Release/CustomTalismanEffects.dll` — a single self-contained DLL (no `libER.dll`/`imgui.dll`/`minhook.dll` to ship).

### Regenerating the talisman list

The embedded name→id table (`src/talisman_names.hpp`) and the default `CustomTalismanEffects.ini` are both generated from the Paramdex names file. To refresh them after a Paramdex update:

```sh
cd tools
curl -L -o EquipParamAccessory.txt \
  https://raw.githubusercontent.com/soulsmods/Paramdex/master/ER/Names/EquipParamAccessory.txt
python gen_talisman_data.py
```

The generator fails loudly if two talismans normalize to the same name (which would make the `.ini` key ambiguous), so the header and the `.ini` always stay in lockstep.

## Install

Copy `CustomTalismanEffects.dll` + `CustomTalismanEffects.ini` into your mod loader's native-mod folder (e.g. the me3 profile folder), launch offline.

### Linux / Steam Deck (Proton)

CustomTalismanEffects remains a Windows x64 DLL under Proton. Load it through
the same Windows native-mod loader inside Elden Ring's Proton prefix. The DLL
and its static dependencies carry the MSVC runtime, so a separate Visual C++
redistributable should not be required merely to load the mod.

Start compatibility testing in SDR with optional overlays disabled. Add
Gamescope HDR/VRR, Steam Overlay, MangoHud, frame generation, and other
injectors one at a time only after the baseline works. Do not place arbitrary
native `dxgi.dll` or `d3d12.dll` copies beside the game.

At startup the log identifies Wine/Proton and the actual DXGI/D3D12 modules in
use. If the mod directory is read-only, logging falls back to
`%TEMP%/CustomTalismanEffects/logs/CustomTalismanEffects.log`. If no log exists
in either location, the DLL did not reach its worker; check the mod loader and
Proton log first. An unreadable `.ini` selects defaults and does not disable
the overlay renderer.

## Logs

Every launch writes `logs/CustomTalismanEffects.log` next to the DLL. A healthy run shows the apply function resolving, params loading, each enabled talisman resolving to its effect ids, and (with `log_each = 1`) each SpEffect as it's applied:

```
2 talisman(s) enabled in the .ini
apply function resolved: entry=00007FF6...
params ready -- resolving talisman effects
talisman "Erdtree's Favor +2" (acc 1042) -> 1 effect(s)
talisman "Green Turtle Talisman" (acc 1150) -> 1 effect(s)
entering apply loop (2 talisman(s))
applied SpEffect ... (Erdtree's Favor +2)
```

If the log says the `ApplySpEffect` AOB wasn't found (or matched multiple sites), the signature has drifted for your game version and needs updating in `src/offsets.hpp`. If a talisman resolves to `0 effect(s)` or "not in this regulation", that row is empty or belongs to DLC you don't own.
