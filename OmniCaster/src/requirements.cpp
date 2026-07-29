#include "requirements.hpp"

#include <algorithm>
#include <vector>

// libER param tables
#include <param/param.hpp>

#include "utils.hpp"

namespace {

using omni::flog;

// A spell's BASELINE casting-stat requirement -- the pair we flip from. Kept so
// every flip recomputes from the baseline instead of from the previously
// flipped row (which would otherwise lose the losing stat's number after the
// first flip).
//
// `last_*` is what OmniCaster itself last wrote into the row. If the live row
// ever disagrees with it, somebody else edited these two fields (in practice:
// AdjustableSpellCost dividing every requirement) and their values become the
// new baseline -- see sync_requirements().
struct ReqEntry {
    int           id;
    unsigned char req_int;   // baseline INT requirement
    unsigned char req_fai;   // baseline FAI requirement
    unsigned char last_int;  // what we last wrote (== baseline before first write)
    unsigned char last_fai;
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
                              row.requirementFaith, row.requirementIntellect,
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

void sync_requirements(bool use_int) {
    int rebaselined = 0, written = 0;

    for (auto& e : g_req_rows) {
        auto [row, ok] = from::param::Magic[e.id];
        if (!ok) continue;

        const unsigned char live_int = row.requirementIntellect;
        const unsigned char live_fai = row.requirementFaith;

        // Sibling-mod compatibility (AdjustableSpellCost's requirement
        // divider). Both mods write these two fields, both load on their own
        // thread, and neither can order the other -- so instead of assuming who
        // ran first, we notice. Anything in the row that we did not put there
        // came from the other mod and is adopted as the new baseline; the flip
        // then rides on top of it. Whichever order the two passes happen in,
        // this converges within one tick to "divided number, on the winning
        // stat". Hybrid INT+FAI spells still collapse to max() of whatever the
        // other mod left, which is the same answer as dividing max().
        if (live_int != e.last_int || live_fai != e.last_fai) {
            e.req_int = live_int;
            e.req_fai = live_fai;
            ++rebaselined;
        }

        // The requirement NUMBER is preserved; only which stat it reads from
        // changes. Spells needing both stats collapse onto the larger of the
        // two (the smaller one is already covered by definition).
        const unsigned char req   = std::max(e.req_int, e.req_fai);
        const unsigned char n_int = use_int ? req : 0;
        const unsigned char n_fai = use_int ? 0   : req;

        if (n_int != live_int || n_fai != live_fai) {
            row.requirementIntellect = n_int;
            row.requirementFaith     = n_fai;
            // requirementLuck (Arcane) stays exactly as the game had it.
            ++written;
        }
        e.last_int = n_int;
        e.last_fai = n_fai;
    }

    if (rebaselined)
        flog("requirement swap: adopted an outside edit on %d spell row(s) "
             "(another mod -- e.g. AdjustableSpellCost -- changed INT/FAI "
             "requirements); %d row(s) rewritten onto %s",
             rebaselined, written, use_int ? "INT" : "FAI");
}

size_t requirement_row_count() { return g_req_rows.size(); }

} // namespace omni
