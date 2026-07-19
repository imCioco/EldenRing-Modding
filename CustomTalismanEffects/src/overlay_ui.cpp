// The talisman panel -- the half of the overlay the player actually sees.
//
// Backend-agnostic by design (see overlay_ui.hpp): no DXGI, no D3D12, no raw
// input, no pad detours. It gets a ui::Frame each frame and returns a
// ui::Result. Everything else it needs is g_state, exactly as before the
// backend v2 split -- the panel's markup, layout, wording and behavior are
// unchanged from the previous separate-window overlay, with one addition: a
// Close button in the top-right corner.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <Xinput.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>

#include "overlay_ui.hpp"
#include "state.hpp"
#include "log.hpp"

#pragma comment(lib, "shell32.lib")

namespace cte::overlay::ui {

namespace {

// Mod page opened by the small "Nexus" button in the overlay. Leave empty ("")
// to hide the button entirely.
constexpr const char* kModPageUrl = "https://www.nexusmods.com/eldenring/mods/10327";

// ── per-open panel state ──
// File-scope rather than function-static so reset_on_close() can clear the bits
// that must not survive a close (the Close latch, the pad-shoulder edge
// detector). The search text deliberately DOES persist across opens, as it
// always has.
bool g_close_requested = false;
char g_filter[64] = "";
int  g_import_sel = 0;
bool g_prev_lb = false, g_prev_rb = false;

// ── resolution-based overlay scale ──
// The panel was authored against a ~1440p canvas; on 4K screens its 18px type
// and 440px width were far too small. Everything scales by
// canvas_height / 1440 (clamped to 1x-2x, so 1080p keeps the authored look).
// Applied WITHOUT rebuilding the font atlas: FontGlobalScale samples the same
// oversampled glyphs at draw time (Latin stays crisp, merged CJK renders a
// touch softer -- the atlas-height ceiling forbids larger CJK bakes). Style
// metrics scale via ScaleAllSizes from a fresh apply_style() base; the panel's
// explicit pixel constants multiply g_ui_scale at their call sites.
// [overlay] ui_scale in the ini forces a value (0 = auto). Recomputed at each
// menu open -- cheap, and it tracks display/resolution changes.
float g_ui_scale = 1.0f;

// The saved [panel] geometry is applied once per session; after that ImGui owns
// the window's size/position and we only write it back.
bool g_panel_layout_applied = false;

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// Build the ImGui font atlas: a Latin base font for the UI chrome, with EVERY
// available CJK font merged on top so talisman names in any language render
// correctly -- rather than picking a single "first found" CJK font and using
// it for all CJK text regardless of script (the earlier approach: Malgun
// Gothic, a Korean-only font present on nearly every Windows install, got
// found first and was used for Simplified Chinese too, which it doesn't
// cover -> tofu/mojibake for CN players).
//
// ImGui's glyph lookup is last-added-wins for codepoints that appear in more
// than one merged range (CJK Unified Ideographs are shared by the SC/TC/JP
// ranges, each with its own font-specific stroke shapes). We use the
// player's Windows UI language to decide which font is merged LAST -- i.e.
// whose glyph shapes win any overlapping codepoints -- while still merging
// every other script first as a fallback, so text in a non-primary language
// (a renamed talisman from a translation mod, etc.) still renders instead of
// being skipped.
void load_fonts(ImGuiIO& io) {
    // Cyrillic (Russian, Bulgarian, Ukrainian, Serbian, etc.) glyphs live in the
    // same font FILES as Latin, but AddFontFromFileTTF's default range is Basic
    // Latin + Latin-1 only (0x0020-0x00FF) -- Cyrillic (0x0400+) is never pulled
    // into the atlas unless explicitly requested, even though segoeui/arial/tahoma
    // all contain those glyphs. GetGlyphRangesCyrillic() is a superset of the
    // default Latin range, so this is a straight upgrade with no separate font.
    const char* latin_fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                 "C:\\Windows\\Fonts\\arial.ttf",
                                 "C:\\Windows\\Fonts\\tahoma.ttf"};
    ImFont* base = nullptr;
    for (const char* fp : latin_fonts) {
        if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) {
            base = io.Fonts->AddFontFromFileTTF(fp, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            if (base) break;
        }
    }
    if (!base) base = io.Fonts->AddFontDefault();

    struct CjkFont { const char* lang; const char* path; const ImWchar* ranges; };
    const CjkFont cjk_fonts[] = {
        {"ja", "C:\\Windows\\Fonts\\meiryo.ttc",   io.Fonts->GetGlyphRangesJapanese()},
        {"ja", "C:\\Windows\\Fonts\\msgothic.ttc", io.Fonts->GetGlyphRangesJapanese()},
        {"ko", "C:\\Windows\\Fonts\\malgun.ttf",   io.Fonts->GetGlyphRangesKorean()},
        {"zh", "C:\\Windows\\Fonts\\msyh.ttc",     io.Fonts->GetGlyphRangesChineseFull()},
        {"zh", "C:\\Windows\\Fonts\\msjh.ttc",     io.Fonts->GetGlyphRangesChineseFull()},
    };

    // Player's Windows UI language, so overlapping CJK codepoints render with
    // the glyph shapes that language's readers actually expect (the game's
    // own text language usually follows it).
    const char* preferred = nullptr;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE: preferred = "ja"; break;
        case LANG_KOREAN:   preferred = "ko"; break;
        case LANG_CHINESE:  preferred = "zh"; break;
        default: break;
    }

    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    for (const auto& f : cjk_fonts) // fallback scripts first...
        if ((!preferred || std::strcmp(f.lang, preferred) != 0) &&
            GetFileAttributesA(f.path) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(f.path, 18.0f, &merge_cfg, f.ranges);
    for (const auto& f : cjk_fonts) // ...preferred script last, so it wins ties
        if (preferred && std::strcmp(f.lang, preferred) == 0 &&
            GetFileAttributesA(f.path) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(f.path, 18.0f, &merge_cfg, f.ranges);
}

// ── ER-flavored dark/gold theme ──
void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = s.Colors;
    const ImVec4 gold(0.80f, 0.68f, 0.40f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.04f, 0.96f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.10f, 0.05f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.44f, 0.22f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.32f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.42f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.12f, 0.07f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.20f, 0.11f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.88f, 0.78f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.40f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.50f, 0.42f, 0.25f, 0.6f);
    c[ImGuiCol_NavHighlight] = gold;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.24f, 0.13f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.22f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_TabActive] = ImVec4(0.35f, 0.28f, 0.14f, 1.0f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.10f, 0.05f, 1.0f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.50f, 0.42f, 0.25f, 0.6f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.65f, 0.53f, 0.30f, 0.8f);
    c[ImGuiCol_SeparatorActive] = gold;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.24f, 0.12f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.36f, 0.18f, 0.8f);
    c[ImGuiCol_ResizeGripActive] = gold;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.44f, 0.22f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.44f, 0.22f, 0.5f);
}

void update_scale(int canvas_h) {
    float configured = 0.0f;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        configured = g_state.ui_scale;
    }
    const float scale = configured > 0.0f
        ? std::clamp(configured, 0.5f, 2.0f)
        : (canvas_h > 0 ? std::clamp(static_cast<float>(canvas_h) / 1440.0f, 1.0f, 2.0f)
                        : g_ui_scale);
    if (scale == g_ui_scale) return;
    g_ui_scale = scale;
    // Full reset first: ScaleAllSizes multiplies IN PLACE, and apply_style only
    // overrides the fields we theme -- without the reset, the untouched metrics
    // (IndentSpacing, ItemInnerSpacing, MouseCursorScale, ...) would compound
    // across successive scale changes (e.g. a mid-session resolution switch).
    ImGui::GetStyle() = ImGuiStyle(); // defaults (metrics + dark colors)
    apply_style();                    // our theme on top, at base metrics
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
    flog("[overlay] ui scale %.2f (canvas height %d)", scale, canvas_h);
}

// ── the talisman panel ──
Result draw(const Frame& f) {
    Result r;

    std::lock_guard<std::mutex> lk(g_state_mutex);

    // Panel geometry: apply the ini-saved size/position once per session,
    // clamped into the current canvas (a save from another resolution must
    // still land on-screen). No saved values -> the scaled default, placed by
    // ImGui as before. Afterwards ImGui owns the geometry across opens.
    if (!g_panel_layout_applied) {
        g_panel_layout_applied = true;
        const float cw = f.canvas_w, ch = f.canvas_h;
        if (g_state.panel_w >= 200.0f && g_state.panel_h >= 200.0f && cw > 0 && ch > 0) {
            const float w = std::min(g_state.panel_w, cw);
            const float h = std::min(g_state.panel_h, ch);
            const float x = std::clamp(g_state.panel_x, 0.0f, cw - w);
            const float y = std::clamp(g_state.panel_y, 0.0f, ch - h);
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        } else {
            ImGui::SetNextWindowSize(ImVec2(440.0f * g_ui_scale, 580.0f * g_ui_scale),
                                     ImGuiCond_FirstUseEver);
        }
    }

    if (!ImGui::Begin("Custom Talisman Effects", nullptr)) {
        ImGui::End();
        r.close_requested = g_close_requested;
        g_close_requested = false;
        return r;
    }

    // Capture the live geometry for the ini ([panel]); saved on menu close.
    {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        g_state.panel_x = wp.x;
        g_state.panel_y = wp.y;
        g_state.panel_w = ws.x;
        g_state.panel_h = ws.y;
    }

    // Shared gold accent (matches apply_style()'s `gold`).
    const ImVec4 kGold(0.80f, 0.68f, 0.40f, 1.0f);

    // ── Header: emphasized title + corner buttons, set off by a separator ──
    ImGui::SetWindowFontScale(1.15f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted("Custom Talisman Effects");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    // Close sits in the exact top-right corner, on the title's line; Nexus
    // (the mod-page link, opens the default browser) sits just to its left.
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const char* close_label = "Close";
        const float close_w = ImGui::CalcTextSize(close_label).x + style.FramePadding.x * 2.0f;
        const bool show_nexus = kModPageUrl[0] != '\0';
        const float nexus_w = show_nexus
            ? ImGui::CalcTextSize("Nexus").x + style.FramePadding.x * 2.0f : 0.0f;
        const float right_edge = ImGui::GetWindowContentRegionMax().x;
        if (show_nexus) {
            ImGui::SameLine(right_edge - close_w - style.ItemSpacing.x - nexus_w);
            if (ImGui::SmallButton("Nexus"))
                ShellExecuteA(nullptr, "open", kModPageUrl, nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
        } else {
            ImGui::SameLine(right_edge - close_w);
        }
        if (ImGui::SmallButton(close_label))
            g_close_requested = true; // applied by the backend right after draw()
    }
    ImGui::Separator();
    ImGui::Spacing();

    // ── Import prompt: a new character with no saved preset, shown only when
    //    OTHER characters already have presets to import from (set by the worker).
    if (g_state.import_prompt_active && !g_state.import_candidates.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.12f, 0.05f, 0.85f));
        ImGui::BeginChild("##import_banner", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border);
        ImGui::TextColored(kGold, "New character - no saved preset");
        ImGui::TextWrapped("Start fresh (all talismans off), or import another "
                           "character's setup:");
        if (g_import_sel >= static_cast<int>(g_state.import_candidates.size())) g_import_sel = 0;
        auto cand_label = [&](int i) {
            const auto& c = g_state.import_candidates[static_cast<size_t>(i)];
            return c.second.empty() ? c.first : c.second; // display name, else key
        };
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##import_from", cand_label(g_import_sel).c_str())) {
            for (int i = 0; i < static_cast<int>(g_state.import_candidates.size()); ++i) {
                const bool is_sel = (i == g_import_sel);
                if (ImGui::Selectable(cand_label(i).c_str(), is_sel)) g_import_sel = i;
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // The worker performs the copy + save (all disk I/O off the render thread);
        // clear the prompt now for immediate feedback.
        if (ImGui::Button("Import selected")) {
            g_state.import_from_key = g_state.import_candidates[static_cast<size_t>(g_import_sel)].first;
            g_state.import_requested = true;
            g_state.import_prompt_active = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Start fresh")) {
            g_state.import_from_key.clear();
            g_state.import_requested = true;
            g_state.import_prompt_active = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Settings: the global toggles, grouped and set off from the list below ──
    bool stacking = g_state.allow_stacking;
    if (ImGui::Checkbox("Allow stacking (ignore talisman families)", &stacking)) {
        g_state.allow_stacking = stacking;
        if (!stacking) collapse_groups_locked();
    }
    bool progression = g_state.progression_mode;
    if (ImGui::Checkbox("Progression mode (owned talismans only)", &progression)) {
        g_state.progression_mode = progression;
        g_state.save_requested = true; // persist the toggle like the others
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-90.0f * g_ui_scale); // reserve room for Clear
    ImGui::InputTextWithHint("##search", "Search talismans...", g_filter, sizeof(g_filter));
    r.search_active = ImGui::IsItemActive();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) g_filter[0] = '\0';

    int on = 0;
    for (const auto& t : g_state.talismans) if (t.enabled) ++on;
    // Status indicator: enabled count in gold, total muted.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGold, "Enabled: %d", on);
    ImGui::SameLine(0.0f, 4.0f * g_ui_scale);
    ImGui::TextDisabled("/ %d", static_cast<int>(g_state.talismans.size()));
    ImGui::SameLine();
    // Save = primary action (gold-tinted). Disable all = secondary (default,
    // only brightening on hover via the theme).
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.42f, 0.34f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.45f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.66f, 0.54f, 0.27f, 1.0f));
    if (ImGui::Button("Save changes")) g_state.save_requested = true;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button("Disable all"))
        for (auto& t : g_state.talismans) t.enabled = false;

    // Talismans currently worn on the character are drawn in this color; when
    // stacking is off they're also greyed out / non-toggleable (already active).
    const ImVec4 kEquippedCol(0.45f, 0.78f, 0.98f, 1.0f);

    ImGui::Separator();

    // Hotkey hint footer text (labels come straight from the .ini). Built up
    // front so its WRAPPED height at the current window width can be reserved.
    // The gamepad combo can be disabled entirely (toggle_gamepad_combo = none in
    // the .ini, open_pad_mask == 0) -- drop it from the hint when that's the case.
    std::string footer = g_state.open_key_label;
    if (f.open_pad_mask) footer += " or " + g_state.open_pad_label;
    footer += ": open/close  |  Esc or B (pad): close & save";
    if (g_state.has_mod_added) footer += "  |  LB/RB: switch tab";
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float footer_h =
        ImGui::CalcTextSize(footer.c_str(), nullptr, false, wrap_w).y + 4.0f * g_ui_scale;

    const float controls_h =
        ImGui::GetFrameHeightWithSpacing() + footer_h + 4.0f * g_ui_scale;
    const float detail_h = g_state.show_descriptions
        ? ImGui::GetTextLineHeightWithSpacing() * 4.0f + 8.0f * g_ui_scale
        : 0.0f;
    const std::string needle = to_lower(g_filter);
    const Talisman* detail = nullptr; // row hovered by mouse OR focused by gamepad nav
    bool detail_equipped = false;

    auto render_rows = [&](bool base_tab) {
        int shown = 0;
        for (size_t i = 0; i < g_state.talismans.size(); ++i) {
            Talisman& t = g_state.talismans[i];
            if (t.is_base != base_tab) continue;
            // Progression mode: hide talismans the player doesn't currently own
            // (fails open until the first good inventory read).
            if (g_state.progression_mode && g_state.possessed_valid &&
                !g_state.possessed_accessories.count(t.accessory_id)) continue;
            if (!needle.empty() && to_lower(t.name).find(needle) == std::string::npos) continue;
            ++shown;

            bool equipped = false;
            for (int sp : t.sp_ids)
                if (g_state.external_active.count(sp)) { equipped = true; break; }
            const bool lock_it = equipped;

            ImGui::PushID(static_cast<int>(i));
            if (equipped) ImGui::PushStyleColor(ImGuiCol_Text, kEquippedCol);
            if (lock_it)  ImGui::BeginDisabled();

            bool v = lock_it ? true : t.enabled; // a locked/equipped row reads as active
            std::string label = t.name;
            if (equipped) {
                label += " (Already equipped)";
            }
            if (ImGui::Checkbox(label.c_str(), &v)) {
                t.enabled = v;
                if (v) apply_exclusivity_locked(i);
            }

            if (lock_it)  ImGui::EndDisabled();
            if (equipped) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                detail = &t;
                detail_equipped = equipped;
            }
            ImGui::PopID();
        }
        if (shown == 0) {
            const bool progression_empty =
                g_state.progression_mode && g_state.possessed_valid && needle.empty();
            ImGui::TextDisabled(
                progression_empty ? "You haven't found any talismans yet."
                : base_tab        ? "No talismans match your search."
                                  : "No mod-added talismans detected in this regulation.");
        }
    };

    if (g_state.has_mod_added) {
        const bool lb = f.pad_connected && (f.pad_buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        const bool rb = f.pad_connected && (f.pad_buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        int select_tab = -1; // -1 = no request; 0 = Base, 1 = Mod-Added
        if (lb && !g_prev_lb) select_tab = 0;
        if (rb && !g_prev_rb) select_tab = 1;
        g_prev_lb = lb;
        g_prev_rb = rb;

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Base Game", nullptr,
                    select_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("##list_base", ImVec2(0, -(controls_h + detail_h)),
                                  ImGuiChildFlags_NavFlattened);
                render_rows(true);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mod-Added", nullptr,
                    select_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("##list_mod", ImVec2(0, -(controls_h + detail_h)),
                                  ImGuiChildFlags_NavFlattened);
                render_rows(false);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::BeginChild("##list_base", ImVec2(0, -(controls_h + detail_h)),
                          ImGuiChildFlags_NavFlattened);
        render_rows(true);
        ImGui::EndChild();
    }

    if (g_state.show_descriptions) {
        ImGui::Separator();
        ImGui::BeginChild("##detail", ImVec2(0, detail_h - 8.0f * g_ui_scale));
        if (detail) {
            ImGui::TextColored(detail_equipped ? kEquippedCol : ImVec4(0.80f, 0.68f, 0.40f, 1.0f),
                               "%s%s", detail->name.c_str(), detail_equipped ? "  (equipped)" : "");
            ImGui::TextWrapped("%s", detail->effect.empty()
                ? "(no description yet)"
                : detail->effect.c_str());
        } else {
            ImGui::TextDisabled("Hover or select a talisman to see its effect.");
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    static const char* kSortLabels[] = { "Talisman ID", "Name (A-Z)", "In-Game Order" };
    int sort_mode = g_state.sort_mode;
    ImGui::SetNextItemWidth(160.0f * g_ui_scale);
    if (ImGui::Combo("Sort by", &sort_mode, kSortLabels, IM_ARRAYSIZE(kSortLabels))) {
        g_state.sort_mode = sort_mode;
        sort_talismans_locked();
    }
    ImGui::SameLine();
    bool show_desc = g_state.show_descriptions;
    if (ImGui::Checkbox("Show descriptions", &show_desc))
        g_state.show_descriptions = show_desc;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", footer.c_str());
    ImGui::PopStyleColor();

    ImGui::End();

    r.close_requested = g_close_requested;
    g_close_requested = false;
    return r;
}

void reset_on_close() {
    // The search text is intentionally kept (it always survived a close). Only
    // the transient bits are cleared: a Close press the backend has already
    // consumed, and the shoulder-button edge detector -- a button still held
    // physically at close time must not read as a fresh press on reopen.
    g_close_requested = false;
    g_prev_lb = g_prev_rb = false;
}

} // namespace cte::overlay::ui
