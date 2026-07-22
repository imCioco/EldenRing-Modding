#pragma once

#include <string>

namespace pb {

// ---- paths / logging (logs/PersistentBuffs.log next to the DLL) --------
std::wstring module_path();
std::wstring dir_of(const std::wstring& p);
std::wstring stem_of(const std::wstring& p);
std::wstring config_path();
// Cross-session state file next to the DLL: <dll dir>\PersistentBuffs.state.ini.
// Machine-written (see session_store); safe to edit or delete.
std::wstring state_path();
std::wstring log_path();
// Optional Paramdex names file next to the DLL, e.g. from soulsmods/Paramdex
// ER/Names/SpEffectParam.txt (lines "id name"). Absent -> log uses bare ids.
std::wstring names_path();

// Append one timestamped line to the log. Multi-line messages ('\n' inside)
// are aligned under the timestamp; an empty msg writes a blank spacer line.
void log_line(const std::string& msg, bool truncate = false);
void flog(const char* fmt, ...);
// Blank line + "----[ title ]------" divider: marks a phase change (startup
// done, death/fast-travel transition) so a human can scan the log by section.
void flog_section(const char* fmt, ...);
// "12.3s" / "4m05s" / "1h02m" / "infinite" -- human-readable durations.
std::string fmt_secs(double s);

} // namespace pb
