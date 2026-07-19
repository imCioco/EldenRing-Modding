#pragma once

// ── multi-mod coexistence: every process-wide name this backend claims ─────
//
// Ported from QuestPath's backend v2 (see its .claude/OVERLAY_V2_PORTING_GUIDE
// .md, "Two copies of this backend in ONE game process"). This backend is
// embedded in more than one mod that can be loaded into the same game process
// -- CustomTalismanEffects and QuestPath ship the same renderer -- so
// everything that is a PROCESS-WIDE singleton lives here, giving a port exactly
// one file to review.
//
// There are two kinds of name, and they must be treated oppositely:
//
//   SHARED  (kHookInstallMutex in hooks.hpp, kMenuOwnerMutex below)
//           Arbitration primitives. Every mod embedding this backend must use
//           the IDENTICAL string or the arbitration does nothing. NEVER
//           rename these -- not even to something CTE-flavored. They are
//           versioned instead (".v1"), and they keep the "QPBackend" prefix
//           because that is the string QuestPath already ships; renaming here
//           would silently un-arbitrate both mods.
//
//   PRIVATE (kInputWindowClass below)
//           Per-mod resources that must NOT collide. RegisterClassExW fails
//           with ERROR_CLASS_ALREADY_EXISTS if a second mod registers the same
//           class name in the same process, which kills its input sink and
//           therefore all keyboard/mouse capture.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace cte::coexist {

// PRIVATE. Everything below derives from it. Identifier-safe (no spaces or
// backslashes) -- it goes into a window class name.
constexpr const wchar_t* kModName = L"CustomTalismanEffects";

// PRIVATE. Message-only HWND that owns our raw-input registration. Must differ
// from QuestPath's "QuestPath_InputSink_V2".
constexpr const wchar_t* kInputWindowClass = L"CustomTalismanEffects_InputSink_V2";

// ── menu ownership ────────────────────────────────────────────────────────
// Raw input is a per-process singleton: exactly one HWND receives keyboard and
// mouse. Each overlay snapshots "whatever was registered" at open and restores
// it at close. With two overlays open at once that snapshot is wrong -- the
// second one snapshots the FIRST overlay's sink hwnd instead of the game's, so
// a close-order inversion restores raw input to a gated sink and the game
// loses keyboard and mouse entirely. Cursor-clip replay has the same shape:
// each close replays "the game's last wanted clip", re-pinning the cursor out
// from under an overlay that is still open.
//
// Rather than teach the snapshot/restore code about peers, only one overlay in
// the process may be open at a time. First requester wins; a second request is
// refused (and told why) until the first one closes.
//
// Non-blocking. Must be released on the SAME thread that acquired it -- a
// Windows mutex is thread-affine. Both calls belong on the overlay thread's
// open/close edge.
bool acquire_menu_ownership();
void release_menu_ownership();

// True while this instance holds ownership. Cheap; safe to call per frame.
bool holds_menu_ownership();

} // namespace cte::coexist
