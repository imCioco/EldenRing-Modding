#pragma once

#include <string>
#include <vector>

namespace iwb {

// ---- Paramdex human names (readable logs) ------------------------------
// The soulsmods/Paramdex ER name lists (SpEffectParam / EquipParamGoods /
// Magic) are EMBEDDED in the DLL at build time (external/Paramdex submodule ->
// cmake/embed_names.cmake -> byte arrays). load_names() parses them once at
// startup so the log can print "1851:[Weapon] Oath of Vengeance - Damage Buff"
// instead of a bare id. Optional loose <Name>.txt files next to the DLL (e.g.
// SpEffectParam.txt) OVERRIDE/extend the embedded set for overhaul ids Paramdex
// doesn't know. The game's release regulation has these row names stripped, so
// names MUST come from Paramdex -- the paramdef .hpp files are struct layouts,
// not names.

// Build the id->name maps. Call once (from the worker thread; blocking file I/O
// for the optional loose overrides). Logs a one-line summary. Returns the
// SpEffect name count.
size_t load_names();

// Format an id for the log: "id" or, if a name is known, "id:Name".
std::string named_speffect(int id);
std::string named_goods(int id);
std::string named_magic(int id);

} // namespace iwb
