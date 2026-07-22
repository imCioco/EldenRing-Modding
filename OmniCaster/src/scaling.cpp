#include "scaling.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

// libER param tables
#include <param/param.hpp>

#include "names.hpp"
#include "player_stats.hpp"
#include "requirements.hpp"
#include "utils.hpp"

namespace {

using omni::Config;
using omni::ScalingMode;
using omni::flog;
using omni::wep_label;

// Catalyst classification, from the row's ORIGINAL cast flags.
enum class Dir {
    MagicToDark, // staff: sorcery (magic) side is real -> mirror onto holy
    DarkToMagic, // seal: incant (holy/"dark") side is real -> mirror onto magic
};

// ---- AttackElementCorrectParam column copy ------------------------------
// A "column" is one damage element's 5 stat-correction entries (bit +
// overwrite rate + influence rate per STR/DEX/INT/FAI/ARC).
using AecRow = from::paramdef::ATTACK_ELEMENT_CORRECT_PARAM_ST;

#define OMNI_COPY_AEC_COLUMN(row, DST, SRC)                                         \
    do {                                                                            \
        (row).isStrengthCorrect_by##DST  = (row).isStrengthCorrect_by##SRC;         \
        (row).isDexterityCorrect_by##DST = (row).isDexterityCorrect_by##SRC;        \
        (row).isMagicCorrect_by##DST     = (row).isMagicCorrect_by##SRC;            \
        (row).isFaithCorrect_by##DST     = (row).isFaithCorrect_by##SRC;            \
        (row).isLuckCorrect_by##DST      = (row).isLuckCorrect_by##SRC;             \
        (row).overwriteStrengthCorrectRate_by##DST  = (row).overwriteStrengthCorrectRate_by##SRC;  \
        (row).overwriteDexterityCorrectRate_by##DST = (row).overwriteDexterityCorrectRate_by##SRC; \
        (row).overwriteMagicCorrectRate_by##DST     = (row).overwriteMagicCorrectRate_by##SRC;     \
        (row).overwriteFaithCorrectRate_by##DST     = (row).overwriteFaithCorrectRate_by##SRC;     \
        (row).overwriteLuckCorrectRate_by##DST      = (row).overwriteLuckCorrectRate_by##SRC;      \
        (row).InfluenceStrengthCorrectRate_by##DST  = (row).InfluenceStrengthCorrectRate_by##SRC;  \
        (row).InfluenceDexterityCorrectRate_by##DST = (row).InfluenceDexterityCorrectRate_by##SRC; \
        (row).InfluenceMagicCorrectRate_by##DST     = (row).InfluenceMagicCorrectRate_by##SRC;     \
        (row).InfluenceFaithCorrectRate_by##DST     = (row).InfluenceFaithCorrectRate_by##SRC;     \
        (row).InfluenceLuckCorrectRate_by##DST      = (row).InfluenceLuckCorrectRate_by##SRC;      \
    } while (0)

void mirror_aec(AecRow& row, Dir dir) {
    if (dir == Dir::MagicToDark)
        OMNI_COPY_AEC_COLUMN(row, Dark, Magic);
    else
        OMNI_COPY_AEC_COLUMN(row, Magic, Dark);
}

// ---- highest-stat mode state --------------------------------------------
// Post-mirror snapshots of every catalyst AEC row we own, so stat flips are
// idempotent (each flip recomputes from the snapshot, not the live row).
struct FlipEntry {
    int    id;
    AecRow snap; // state right after the mirror pass
};
std::vector<FlipEntry> g_flip_rows;

// Retarget one element column of a live AEC row to a single casting stat.
// `use_int` picks INT vs FAI. Only the INT/FAI entries are touched: STR/DEX/
// ARC corrections (e.g. Dragon Communion Seal's arcane) keep their snapshot
// values via the mirror pass and stay as the game had them.
#define OMNI_FLIP_COLUMN(row, snap, E, use_int)                                     \
    do {                                                                            \
        const bool had_any = (snap).isMagicCorrect_by##E || (snap).isFaithCorrect_by##E; \
        const short infl = std::max((snap).InfluenceMagicCorrectRate_by##E,         \
                                    (snap).InfluenceFaithCorrectRate_by##E);        \
        short ow = -1;                                                              \
        if ((snap).isMagicCorrect_by##E && (snap).isFaithCorrect_by##E)             \
            ow = std::max((snap).overwriteMagicCorrectRate_by##E,                   \
                          (snap).overwriteFaithCorrectRate_by##E);                  \
        else if ((snap).isMagicCorrect_by##E)                                       \
            ow = (snap).overwriteMagicCorrectRate_by##E;                            \
        else if ((snap).isFaithCorrect_by##E)                                       \
            ow = (snap).overwriteFaithCorrectRate_by##E;                            \
        (row).isMagicCorrect_by##E = had_any && (use_int);                          \
        (row).isFaithCorrect_by##E = had_any && !(use_int);                         \
        if (use_int) {                                                              \
            (row).InfluenceMagicCorrectRate_by##E = infl;                           \
            (row).overwriteMagicCorrectRate_by##E = ow;                             \
        } else {                                                                    \
            (row).InfluenceFaithCorrectRate_by##E = infl;                           \
            (row).overwriteFaithCorrectRate_by##E = ow;                             \
        }                                                                           \
    } while (0)

void flip_all(bool use_int) {
    for (auto& e : g_flip_rows) {
        auto [row, ok] = from::param::AttackElementCorrectParam[e.id];
        if (!ok) continue;
        OMNI_FLIP_COLUMN(row, e.snap, Magic, use_int);
        OMNI_FLIP_COLUMN(row, e.snap, Dark, use_int);
    }
}

} // namespace

namespace omni {

void apply_all(const Config& cfg) {
    // ---- spell requirements (Magic param, independent of scaling_mode) ----
    // Done first: the scaling passes below bail out early in `off` mode.
    if (cfg.requirement_swap) snapshot_requirements(cfg.dump);

    // ---- pass 1: classify every weapon row, collect shared-table usage ----
    struct Catalyst {
        int id;
        Dir dir;
    };
    std::vector<Catalyst> catalysts;
    // AEC / reinforce ids referenced by catalysts (per direction) and by
    // everything else -- a shared row must not be patched blindly.
    std::map<int, Dir> aec_by_catalyst;
    std::set<int>      aec_conflicted; // claimed by both directions
    std::set<int>      aec_by_other;
    std::map<int, Dir> reinf_by_catalyst;
    std::set<int>      reinf_conflicted;
    std::set<int>      reinf_by_other;

    for (auto [id, row] : from::param::EquipParamWeapon) {
        const bool magic   = row.enableMagic;
        const bool miracle = row.enableMiracle;
        if (magic == miracle) { // neither (normal weapon) or both (already modded)
            if (magic && miracle)
                // Normal for natively dual catalysts (Staff of the Great
                // Beyond; many overhaul-mod catalysts): they already cast
                // both and their scaling is the designer's -- leave alone.
                flog("%s casts both natively -- left untouched",
                     wep_label(static_cast<int>(id)).c_str());
            reinf_by_other.insert(row.reinforceTypeId);
            aec_by_other.insert(row.attackElementCorrectId);
            continue;
        }
        const Dir dir = magic ? Dir::MagicToDark : Dir::DarkToMagic;
        catalysts.push_back({static_cast<int>(id), dir});

        auto claim = [dir](std::map<int, Dir>& owned, std::set<int>& conflicted, int key) {
            auto [it, inserted] = owned.emplace(key, dir);
            if (!inserted && it->second != dir) conflicted.insert(key);
        };
        claim(aec_by_catalyst, aec_conflicted, row.attackElementCorrectId);
        claim(reinf_by_catalyst, reinf_conflicted, row.reinforceTypeId);
    }

    flog("found %zu catalyst rows (%zu AEC rows, %zu reinforce types)",
         catalysts.size(), aec_by_catalyst.size(), reinf_by_catalyst.size());
    if (catalysts.empty()) {
        flog("[WARN] no staffs/seals found -- libER/version mismatch?");
        return;
    }

    // ---- pass 2: weapon-level edits ----
    int crossed = 0, mirrored = 0;
    for (auto& c : catalysts) {
        auto [row, ok] = from::param::EquipParamWeapon[c.id];
        if (!ok) continue;
        const bool staff = c.dir == Dir::MagicToDark;

        if (cfg.cast_anything) {
            row.enableMagic    = true;
            row.enableMiracle  = true;
            row.enableVowMagic = true;
            ++crossed;
        }
        if (cfg.mode != ScalingMode::Off) {
            const unsigned short oldMag = row.attackBaseMagic;
            const unsigned short oldDrk = row.attackBaseDark;
            if (staff) {
                row.attackBaseDark   = row.attackBaseMagic;
                row.correctType_Dark = row.correctType_Magic;
            } else {
                row.attackBaseMagic   = row.attackBaseDark;
                row.correctType_Magic = row.correctType_Dark;
            }
            if (cfg.mode == ScalingMode::HighestStat) {
                const float best = std::max(row.correctMagic, row.correctFaith);
                row.correctMagic = best;
                row.correctFaith = best;
            }
            ++mirrored;
            if (cfg.dump)
                flog("[dump] %-42s %s  atkMag %u->%u atkHoly %u->%u  aec=%d reinf=%d",
                     wep_label(c.id).c_str(), staff ? "STAFF" : "SEAL ",
                     oldMag, row.attackBaseMagic, oldDrk, row.attackBaseDark,
                     row.attackElementCorrectId, row.reinforceTypeId);
        }
    }
    flog("cross-casting enabled on %d catalysts; scaling mirrored on %d", crossed, mirrored);

    if (cfg.mode == ScalingMode::Off) return;

    // ---- pass 3: AttackElementCorrectParam (stat -> element correction) ----
    g_flip_rows.clear();
    int aec_patched = 0;
    for (auto& [aid, dir] : aec_by_catalyst) {
        auto [row, ok] = from::param::AttackElementCorrectParam[aid];
        if (!ok) {
            flog("[WARN] AEC row %d missing -- skipped", aid);
            continue;
        }
        if (aec_by_other.count(aid)) {
            flog("[WARN] AEC row %d is shared with non-catalyst weapons -- skipped "
                 "(patching it would change their melee scaling)", aid);
            continue;
        }
        if (aec_conflicted.count(aid)) {
            // Same correction row used by both a staff and a seal: merge by
            // mirroring both ways (columns become the union of the two).
            flog("[WARN] AEC row %d used by both a staff and a seal -- merging both columns", aid);
            mirror_aec(row, Dir::MagicToDark);
            mirror_aec(row, Dir::DarkToMagic);
        } else {
            mirror_aec(row, dir);
        }
        ++aec_patched;
        if (cfg.mode == ScalingMode::HighestStat)
            g_flip_rows.push_back({aid, row}); // snapshot AFTER the mirror
        if (cfg.dump)
            flog("[dump] AEC %d mirrored: magic[INT=%d FAI=%d] holy[INT=%d FAI=%d]",
                 aid, (int)row.isMagicCorrect_byMagic, (int)row.isFaithCorrect_byMagic,
                 (int)row.isMagicCorrect_byDark, (int)row.isFaithCorrect_byDark);
    }

    // ---- pass 4: ReinforceParamWeapon (upgrade-level growth) ----
    // Rows for a reinforce type are contiguous: base+0 .. base+maxlevel.
    int reinf_patched = 0;
    std::set<int> reinf_done;
    for (auto& [base, dir] : reinf_by_catalyst) {
        for (int k = 0; k <= 25; ++k) {
            const int rid = base + k;
            if (!reinf_done.insert(rid).second) continue;
            auto [row, ok] = from::param::ReinforceParamWeapon[rid];
            if (!ok) {
                if (k == 0) flog("[WARN] reinforce row %d missing", rid);
                break; // levels are contiguous -- past the end of this type
            }
            if (row.magicAtkRate == row.darkAtkRate) continue; // already uniform
            if (reinf_by_other.count(rid)) {
                flog("[WARN] reinforce row %d shared with non-catalyst weapons -- skipped", rid);
                continue;
            }
            if (dir == Dir::MagicToDark && !reinf_conflicted.count(base))
                row.darkAtkRate = row.magicAtkRate;
            else if (dir == Dir::DarkToMagic && !reinf_conflicted.count(base))
                row.magicAtkRate = row.darkAtkRate;
            else { // both directions claim it: equalize upward
                const float best = std::max(row.magicAtkRate, row.darkAtkRate);
                row.magicAtkRate = best;
                row.darkAtkRate  = best;
            }
            ++reinf_patched;
        }
    }
    flog("patched %d AEC rows, %d reinforce rows", aec_patched, reinf_patched);

    if (cfg.mode == ScalingMode::HighestStat)
        flog("highest-stat mode armed: %zu AEC rows will follow max(INT, FAI)",
             g_flip_rows.size());
}

void highest_stat_tick(const Config& cfg) {
    const bool flip_catalysts =
        cfg.mode == ScalingMode::HighestStat && !g_flip_rows.empty();
    const bool flip_spell_reqs = cfg.requirement_swap && requirement_row_count();
    if (!flip_catalysts && !flip_spell_reqs) return;

    static bool have_choice = false;
    static bool use_int     = true;
    static CasterStats previous{};
    static bool have_previous = false;

    CasterStats st;
    if (!read_caster_stats(st))
        return; // main menu / offsets wrong -- keep whatever is active

    const bool character_changed =
        !have_previous || st.player_ins != previous.player_ins ||
        st.player_game_data != previous.player_game_data ||
        st.base_int != previous.base_int || st.base_fai != previous.base_fai;
    const bool current_stats_changed =
        !have_previous || st.eff_int != previous.eff_int ||
        st.eff_fai != previous.eff_fai;

    if (cfg.dump && character_changed)
        dump_stat_block();

    const bool want_int = st.eff_int >= st.eff_fai; // tie -> INT
    const bool choice_changed = !have_choice || want_int != use_int;

    if (choice_changed) {
        use_int = want_int;
        have_choice = true;
        if (flip_catalysts)  flip_all(use_int);
        if (flip_spell_reqs) flip_requirements(use_int);
    }

    if (choice_changed || current_stats_changed || character_changed) {
        flog("highest-stat: current INT=%d (base %d%+d) FAI=%d (base %d%+d) -> %s%s",
             st.eff_int, st.base_int, st.bonus_int,
             st.eff_fai, st.base_fai, st.bonus_fai,
             use_int ? "INT" : "FAI",
             choice_changed ? (flip_catalysts && flip_spell_reqs
                                   ? " [catalysts + spell requirements updated]"
                               : flip_catalysts ? " [catalysts updated]"
                                                : " [spell requirements updated]")
                            : " [choice unchanged]");
    }

    previous = st;
    have_previous = true;
}

} // namespace omni
