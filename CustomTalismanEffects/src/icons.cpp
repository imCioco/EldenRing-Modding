#include "icons.hpp"

#include <unordered_map>

// libER
#include <param/param.hpp>

namespace cte {
namespace icons {
namespace {

constexpr int kNoIcon = -1; // Paramdex: iconId, -1 == no icon needed

// SpEffect id -> the row's ORIGINAL iconId. Only rows we actually changed.
std::unordered_map<int, int> g_patched;

// Looked up every call rather than cached: operator[] binary-searches the table
// the game CURRENTLY has loaded, so a param reload can't dangle a row pointer.
from::paramdef::SP_EFFECT_PARAM_ST* row(int id) {
    if (id <= 0) return nullptr;
    auto [r, ok] = from::param::SpEffectParam[id];
    return ok ? &r : nullptr;
}

} // namespace

bool hide(int sp_id) {
    if (g_patched.count(sp_id)) return false;
    auto* r = row(sp_id);
    if (!r) return false;
    const int original = r->iconId;
    if (original == kNoIcon) return false; // draws no icon; nothing to undo later
    g_patched.emplace(sp_id, original);
    // One naturally-aligned 4-byte store. iconId is the FIRST field of the row,
    // so the game's render thread reads either the old id or "no icon" -- never
    // a torn value, and no other field's meaning depends on this one. That is
    // why this needs no lock and no whole-row copy.
    r->iconId = kNoIcon;
    return true;
}

bool restore(int sp_id) {
    const auto it = g_patched.find(sp_id);
    if (it == g_patched.end()) return false;
    auto* r = row(sp_id);
    // Only write back if the row still holds OUR value: if a param reload or
    // another mod changed it since, that value wins.
    const bool wrote = r && r->iconId == kNoIcon;
    if (wrote) r->iconId = it->second;
    g_patched.erase(it);
    return wrote;
}

std::size_t restore_all() {
    const std::size_t n = g_patched.size();
    for (const auto& [id, original] : g_patched) {
        auto* r = row(id);
        if (r && r->iconId == kNoIcon) r->iconId = original;
    }
    g_patched.clear();
    return n;
}

} // namespace icons
} // namespace cte
