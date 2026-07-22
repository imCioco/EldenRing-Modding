#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "utils.hpp"
#include "config.hpp"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace pb {

std::wstring module_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(g_hinst, buf, MAX_PATH);
    return std::wstring(buf);
}
std::wstring dir_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
std::wstring stem_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    std::wstring name = s == std::wstring::npos ? p : p.substr(s + 1);
    const size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos ? name : name.substr(0, d);
}
std::wstring config_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".ini";
}
std::wstring state_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".state.ini";
}
std::wstring log_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\logs\\" + stem_of(m) + L".log";
}
std::wstring names_path() {
    return dir_of(module_path()) + L"\\SpEffectParam.txt";
}

void log_line(const std::string& msg, bool truncate) {
    // Serialized: the ApplySpEffect detour logs from the game's thread while
    // the poll thread logs too -- without this, concurrent opens of the same
    // file could drop or interleave lines.
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    const std::wstring path = log_path();
    CreateDirectoryW(dir_of(path).c_str(), nullptr);
    std::ofstream f(path, std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        char ts[24];
        std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        // Multi-line messages: the first line gets the timestamp, continuation
        // lines are padded to align under it (easier for humans to scan). An
        // empty message writes a truly empty spacer line.
        if (msg.empty()) {
            f << '\n';
        } else {
            size_t start = 0;
            bool first = true;
            while (start <= msg.size()) {
                const size_t nl = msg.find('\n', start);
                const std::string line =
                    msg.substr(start, nl == std::string::npos ? std::string::npos
                                                              : nl - start);
                if (first) f << ts << line << '\n';
                else       f << "               " << line << '\n'; // 15-sp pad = ts width
                first = false;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }
    if (g_debug) std::printf("[PersistentBuffs] %s\n", msg.c_str());
}
void flog(const char* fmt, ...) {
    char buf[2048];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    log_line(buf);
}
void flog_section(const char* fmt, ...) {
    char buf[256];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    std::string div = "----[ ";
    div += buf;
    div += " ]";
    if (div.size() < 60) div += std::string(60 - div.size(), '-');
    log_line("");   // blank spacer -- makes each phase easy to spot
    log_line(div);
}
std::string fmt_secs(double s) {
    char buf[32];
    if (!(s >= 0.0) || s > 359999.0) return "infinite";   // NaN/negative/huge
    if (s < 90.0) { std::snprintf(buf, sizeof(buf), "%.1fs", s); return buf; }
    const int total = static_cast<int>(s + 0.5);
    if (total < 3600) {
        std::snprintf(buf, sizeof(buf), "%dm%02ds", total / 60, total % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%dh%02dm", total / 3600,
                      (total % 3600) / 60);
    }
    return buf;
}

} // namespace pb
