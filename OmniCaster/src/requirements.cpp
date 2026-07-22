#include "requirements.hpp"

#include <algorithm>
#include <vector>

// libER param tables
#include <param/param.hpp>

#include "utils.hpp"

namespace {

using omni::flog;

// A spell's ORIGINAL casting-stat requirement. Kept so every flip recomputes
// from vanilla values instead of from the previously flipped row (which would
// otherwise lose the losing stat's number after the first flip).
struct ReqEntry {
    int           id;
    unsigned char req_int;
    unsigned char req_fai;
};
std::vector<ReqEntry> g_req_rows;

} // namespace

namespace omni {

void snapshot_requirements(bool dump) {
    g_req_rows.clear();

    int spells = 0;
    for (auto [id, row] : from::param::Magic) {
        // Every Magic row is a sorcery or incantation; skip truly empty rows.
        const bool has_req = row.requirementIntellect || row.requirementFaith ||
                             row.requirementLuck;
        if (row.mp <= 0 && row.mp_charge <= 0 && !has_req) continue;
        ++spells;

        // Nothing to move for a spell that asks for neither INT nor FAI
        // (e.g. the pure-Arcane Dragon Communion incantations).
        if (!row.requirementIntellect && !row.requirementFaith) continue;

        g_req_rows.push_back({static_cast<int>(id), row.requirementIntellect,
                              row.requirementFaith});
        if (dump)
            flog("[dump] spell %d requires INT %d FAI %d ARC %d",
                 static_cast<int>(id), (int)row.requirementIntellect,
                 (int)row.requirementFaith, (int)row.requirementLuck);
    }

    flog("requirement swap armed: %zu of %d spells will follow max(INT, FAI)",
         g_req_rows.size(), spells);
    if (spells == 0)
        flog("[WARN] no spells found in the Magic param -- possible libER/version mismatch");
}

void flip_requirements(bool use_int) {
    for (auto& e : g_req_rows) {
        auto [row, ok] = from::param::Magic[e.id];
        if (!ok) continue;
        // The requirement NUMBER is preserved; only which stat it reads from
        // changes. Spells needing both stats collapse onto the larger of the
        // two (the smaller one is already covered by definition).
        const unsigned char req = std::max(e.req_int, e.req_fai);
        row.requirementIntellect = use_int ? req : 0;
        row.requirementFaith     = use_int ? 0   : req;
        // requirementLuck (Arcane) stays exactly as the game had it.
    }
}

size_t requirement_row_count() { return g_req_rows.size(); }

} // namespace omni
