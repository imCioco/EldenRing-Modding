// Spell stat requirements: let the player's higher casting stat satisfy a
// spell's INT/FAI requirement without changing the number it asks for.
#pragma once

#include <cstddef>

namespace omni {

// One-shot pass over the Magic param, snapshotting each spell's ORIGINAL
// INT/FAI requirement pair. Call once from apply_all(), before any flip.
void snapshot_requirements(bool dump);

// Move every owned spell's casting-stat requirement onto INT or FAI, and adopt
// any edit another mod made to those two fields in the meantime (see
// requirements.cpp). Safe -- and meant -- to call on EVERY tick, not only when
// the winning stat changes: it only writes rows that actually differ.
// Arcane (requirementLuck) is never touched. No-op if nothing was snapshotted.
void sync_requirements(bool use_int);

// How many spells snapshot_requirements() took ownership of.
size_t requirement_row_count();

} // namespace omni
