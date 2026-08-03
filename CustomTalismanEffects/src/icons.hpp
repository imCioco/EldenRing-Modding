#pragma once

// HUD status-icon suppression. A talisman SpEffect with an `iconId` puts a
// status icon next to the health bar while it's active; blanking that field
// (-1 == the game's own "no icon needed") removes it. Only ever called for the
// effects THIS MOD applied, so a physically worn talisman keeps its icon.
//
// Row ids are remembered, never row POINTERS: every call re-resolves through
// from::param::SpEffectParam so a regulation reload can't leave us writing into
// freed memory. Worker-thread only (run_loop) -- no lock; see icons.cpp.

#include <cstddef>

namespace cte {
namespace icons {

// Blank sp_id's icon, remembering the original. No-op if the row is missing,
// already ours, or already icon-less. Returns true only if it patched.
bool hide(int sp_id);

// Put sp_id's original icon back and forget it. Returns true only if it did.
bool restore(int sp_id);

// Restore every row we patched (the option was turned off). Returns the count.
std::size_t restore_all();

} // namespace icons
} // namespace cte
