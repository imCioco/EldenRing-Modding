#include "game_access.hpp"
#include "offsets.hpp"

namespace pb {

mem::Module g_mod;

uintptr_t get_player_ins() {
    const uintptr_t wcm = mem::deref(g_mod.base + kWorldChrManOffset);
    if (!wcm) return 0;
    return mem::deref(wcm + kPlayerInsOffset);
}

int get_player_hp(uintptr_t player) {
    if (!player) return -1;
    const uintptr_t modules = mem::deref(player + kModulesOffset);
    if (!modules) return -1;
    const uintptr_t data = mem::deref(modules + kDataModuleOffset);
    if (!data) return -1;
    int hp = -1;
    if (!mem::safe_read(data + kHpOffset, hp)) return -1;
    // Trust only values that look like HP; a layout drift reads garbage and
    // must degrade to "unknown", never to a false death.
    if (hp < 0 || hp > 100000) return -1;
    return hp;
}

uintptr_t get_player_game_data() {
    if (!g_gamedataman_var) return 0;
    const uintptr_t gdm = mem::deref(g_gamedataman_var);
    if (!gdm) return 0;
    return mem::deref(gdm + kPlayerGameDataOffset);
}

std::wstring get_character_name() {
    const uintptr_t pgd = get_player_game_data();
    if (!pgd) return std::wstring();
    // Read a fixed POD block (16 name chars + slack) so safe_read guards the
    // fault if the offset is wrong for this build; then terminate + validate.
    struct NameRaw { wchar_t c[17]; };
    NameRaw raw{};
    if (!mem::safe_read(pgd + kCharNameOffset, raw)) return std::wstring();
    std::wstring name;
    for (int i = 0; i < 16; ++i) {
        const wchar_t ch = raw.c[i];
        if (ch == L'\0') break;
        if (ch < 0x20) return std::wstring(); // control char => wrong offset / garbage
        name.push_back(ch);
    }
    return name;
}

int get_active_weapon_id(uintptr_t slot_off, uintptr_t prim_off) {
    const uintptr_t pgd = get_player_game_data();
    if (!pgd) return -1;
    int slot = -1;
    if (!mem::safe_read(pgd + slot_off, slot)) return -1;
    if (slot < 0 || slot > 2) return -1;
    int id = -1;
    if (!mem::safe_read(pgd + prim_off + slot * kWepSlotStride, id)) return -1;
    return id;
}
int get_active_right_weapon_id() {
    return get_active_weapon_id(kCurRightWepSlotOffset, kPrimaryRightWepOffset);
}
int get_active_left_weapon_id() {
    return get_active_weapon_id(kCurLeftWepSlotOffset, kPrimaryLeftWepOffset);
}
int get_arm_style() {
    const uintptr_t pgd = get_player_game_data();
    unsigned char v = 0;
    if (!pgd || !mem::safe_read(pgd + kArmStyleOffset, v)) return -1;
    return v;
}

void enumerate_speffects(uintptr_t player, std::vector<int>& out) {
    out.clear();
    if (!player) return;
    const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
    if (!manager) return;
    uintptr_t slot = mem::deref(manager + kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int id = -1;
        if (mem::safe_read(slot + kSpEffectIdOffset, id) && id > 0)
            out.push_back(id);
        slot = mem::deref(slot + kSpEffectNextOffset);
    }
}

} // namespace pb
