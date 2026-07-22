// Spell stat requirements: let the player's higher casting stat satisfy a
// spell's INT/FAI requirement without changing the number it asks for.
#pragma once

#include <cstddef>

namespace omni {

// One-shot pass over the Magic param, snapshotting each spell's ORIGINAL
// INT/FAI requirement pair. Call once from apply_all(), before any flip.
void snapshot_requirements(bool dump);

// Move every snapshotted spell's casting-stat requirement onto INT or FAI.
// Recomputed from the snapshot each time, so repeated calls are idempotent.
// Arcane (requirementLuck) is never touched. No-op if nothing was snapshotted.
void flip_requirements(bool use_int);

// How many spells snapshot_requirements() took ownership of.
size_t requirement_row_count();

} // namespace omni
