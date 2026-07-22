#pragma once

// Shared logging (logs/<DllName>.log next to the DLL, with a writable temp
// fallback), plus an optional debug console. Used by the worker and overlay.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

namespace cte {

extern HINSTANCE g_hinst; // set in DllMain; used to locate the DLL's folder
extern bool      g_debug; // mirror stdout to a console when true

// Path of the DLL's `.ini` (same folder + base name).
std::wstring config_path();

// Path of the machine-written per-character presets file (same folder + base
// name + ".state.ini"). Distinct from config_path()'s human-edited `.ini`.
std::wstring state_path();

// Append a line to logs/<DllName>.log (truncate to start a fresh log). If that
// location is unavailable, use %TEMP%/<DllName>/logs/<DllName>.log instead.
void log_line(const std::string& msg, bool truncate = false);

// printf-style wrapper around log_line.
void flog(const char* fmt, ...);

} // namespace cte
