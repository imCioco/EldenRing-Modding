#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

namespace cte {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

namespace {

std::wstring module_path() {
    DWORD capacity = 512;
    for (int attempt = 0; attempt < 7; ++attempt) {
        std::vector<wchar_t> buffer(capacity);
        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetModuleFileNameW(g_hinst, buffer.data(), capacity);
        if (length == 0) return {};
        if (length < capacity)
            return std::wstring(buffer.data(), length);
        capacity *= 2;
    }
    return {};
}
std::wstring dir_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
std::wstring stem_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    name = dot == std::wstring::npos ? name : name.substr(0, dot);
    return name.empty() ? L"CustomTalismanEffects" : name;
}
std::wstring preferred_log_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\logs\\" + stem_of(m) + L".log";
}

bool ensure_directory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

std::wstring fallback_log_path() {
    std::vector<wchar_t> buffer(512);
    DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) return {};
    if (length >= buffer.size()) {
        buffer.resize(static_cast<size_t>(length) + 1);
        length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length >= buffer.size()) return {};
    }
    std::wstring root(buffer.data(), length);
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
        root.push_back(L'\\');
    const std::wstring stem = stem_of(module_path());
    root += stem;
    if (!ensure_directory(root)) return {};
    const std::wstring logs = root + L"\\logs";
    if (!ensure_directory(logs)) return {};
    return logs + L"\\" + stem + L".log";
}

bool open_log(FILE** file, const std::wstring& path, bool truncate) {
    if (!file || path.empty()) return false;
    *file = nullptr;
    const std::wstring directory = dir_of(path);
    if (!ensure_directory(directory)) return false;
    return _wfopen_s(file, path.c_str(), truncate ? L"wb" : L"ab") == 0 &&
           *file != nullptr;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}

std::mutex g_log_mutex;
std::wstring g_active_log_path;
bool g_using_fallback = false;
bool g_fallback_notice_written = false;

} // namespace

std::wstring config_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".ini";
}

std::wstring state_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".state.ini";
}

void log_line(const std::string& msg, bool truncate) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* file = nullptr;
    if (g_active_log_path.empty()) {
        const std::wstring preferred = preferred_log_path();
        if (open_log(&file, preferred, truncate)) {
            g_active_log_path = preferred;
        } else {
            const std::wstring fallback = fallback_log_path();
            if (open_log(&file, fallback, truncate)) {
                g_active_log_path = fallback;
                g_using_fallback = true;
            }
        }
    } else {
        open_log(&file, g_active_log_path, truncate);
    }

    if (file) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const auto write_timestamp = [&] {
            std::fprintf(file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
                         st.wSecond, st.wMilliseconds);
        };
        if (g_using_fallback && !g_fallback_notice_written) {
            write_timestamp();
            const std::string path = utf8(g_active_log_path);
            std::fprintf(file,
                         "[WARN] mod-directory log unavailable; using fallback: %s\n",
                         path.empty() ? "<path unavailable>" : path.c_str());
            g_fallback_notice_written = true;
        }
        write_timestamp();
        std::fwrite(msg.data(), 1, msg.size(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
    if (g_debug)
        std::cout << "[CustomTalismanEffects] " << msg << std::endl;
}

void flog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_line(buf);
}

} // namespace cte
