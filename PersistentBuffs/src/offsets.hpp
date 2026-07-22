#pragma once

#include <cstdint>

namespace pb {

// ============================================================
//  VERSION-SPECIFIC OFFSETS / SIGNATURES -- VERIFY before trusting!
//  Seeded from libER's symbol table + community RE. They WILL drift across
//  game patches; treat the values below as starting points and confirm with
//  the in-game enumeration log (and Cheat Engine / Nordgaren's Debug Tool).
//  See CLAUDE.md "Offsets & signatures".
// ============================================================
// WorldChrMan singleton pointer = module_base + this (libER GLOBAL_WorldChrMan).
constexpr uintptr_t kWorldChrManOffset = 0x3D65F88;
// WorldChrMan + this -> local PlayerIns (ChrIns*). CLASSIC value; VERIFY.
constexpr uintptr_t kPlayerInsOffset   = 0x1E508;
// ChrIns + this -> SpecialEffect manager (the SpEffect list owner).
constexpr uintptr_t kSpEffectManagerOffset = 0x178;
// SpEffect manager + this -> first list slot (pointer). NOTE: the active-effect
// list lives one indirection BELOW the manager -- the manager holds a pointer to
// the head slot, it is not the head itself. (Confirmed: TGA CT SpEffect.eraseAll.)
constexpr uintptr_t kSpEffectFirstSlotOffset = 0x8;
// Within a SpEffect list slot:
constexpr uintptr_t kSpEffectIdOffset   = 0x8;   // int  effect id
constexpr uintptr_t kSpEffectNextOffset = 0x30;  // ptr  next slot
// Entry timer block + instance payload (source: vswarte/fromsoftware-rs
// cs::SpecialEffectEntry -- its id/next offsets match the TGA values above,
// which cross-validates the layout):
//   +0x40 f32 removal_timer (engine countdown), +0x44 f32, +0x48 f32 duration
//   (total, -1/0 = infinite), +0x4C f32 interval_timer, +0x50..0x78 payload
//   (cast-time instance state, incl. catalyst-scaled buff magnitude).
constexpr uintptr_t kSpEffectTimerOffset   = 0x40;
constexpr uintptr_t kSpEffectPayloadOffset = 0x50;

// ---- player HP (death detection) ----------------------------------------
// ChrIns + 0x190 -> ChrInsModuleContainer; container + 0x0 -> CSChrDataModule;
// data module + 0x138 -> current HP (int). Source: vswarte/fromsoftware-rs
// (ChrIns::modules / CSChrDataModule::hp) -- the same layout that puts the
// SpEffect manager at ChrIns+0x178, which is confirmed in-game.
constexpr uintptr_t kModulesOffset    = 0x190;
constexpr uintptr_t kDataModuleOffset = 0x0;
constexpr uintptr_t kHpOffset         = 0x138;

// "Apply SpEffect" function: void ApplySpEffect(ChrIns* chr, int id, char unk).
// Resolved by AOB scan; the scanned instruction sits INSIDE the function, so the
// real entry point is `match - kApplySpEffectFuncBackset`. Called with unk=1
// (== "self"), matching TGA CT's SpEffect.addForSelf. See CLAUDE.md.
using ApplySpEffect_t = void(*)(void* chr_ins, int sp_effect_id, char unk);
extern ApplySpEffect_t g_apply;
// Source: The Grand Archives CT, Global Functions/SpEffect_code.cea (add_call).
constexpr const char* kApplySpEffectAob =
    "0f 28 0d ?? ?? ?? ?? ?? 8d ?? ?? 0f 29 ?? ?? ?? 0f b6 d8";
constexpr uintptr_t kApplySpEffectFuncBackset = 0x1D;

// ---- weapon-slot identity (for per-weapon buff memory) -----------------
// GameDataMan singleton, resolved by AOB (the `mov rax,[rip+gdm]` lands at the
// match; the global pointer var = rip-relative @ off 3, len 7). Then:
//   GameDataMan  + 0x08  -> PlayerGameData
//   PlayerGameData+0x32C -> active right-hand slot index (0=Primary/1/2) (int)
//   PlayerGameData+0x328 -> active left-hand  slot index (int)
//   PlayerGameData+0x324 -> ArmStyle byte (0 empty, 1 one-hand, 2 L-2H, 3 R-2H)
//   PlayerGameData+0x5FC + slot*0x8 -> equipped right weapon id (int)
//   PlayerGameData+0x5F8 + slot*0x8 -> equipped left  weapon id (int)
// Source: TGA CT (root .cea baseData; Hero/ChrAsm CurrentWepSlotOffset; ChrAsm 2).
constexpr const char* kGameDataManAob =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";
constexpr uintptr_t kPlayerGameDataOffset  = 0x08;
constexpr uintptr_t kCurRightWepSlotOffset = 0x32C;
constexpr uintptr_t kCurLeftWepSlotOffset  = 0x328;
constexpr uintptr_t kArmStyleOffset        = 0x324;
constexpr uintptr_t kPrimaryRightWepOffset = 0x5FC;
constexpr uintptr_t kPrimaryLeftWepOffset  = 0x5F8;
constexpr uintptr_t kWepSlotStride         = 0x8;
extern uintptr_t g_gamedataman_var; // address of the GameDataMan global pointer

// ---- character name (for cross-session persistence keying) --------------
// PlayerGameData + this -> character name, UTF-16, up to 16 chars + null.
// ⚠ UNVERIFIED (TGA CT community RE). The `session: character key='<Name>_xxxx'`
// log line is the verification step -- confirm it matches the in-game name before
// trusting it. See docs/SESSION_PERSISTENCE_DESIGN.md.
constexpr uintptr_t kCharNameOffset = 0x9C;

} // namespace pb
