#include "runtime_compat.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>

#include "log.hpp"

namespace cte::runtime_compat {
namespace {

using WineStringFn = const char*(__cdecl*)();

std::wstring module_path(HMODULE module) {
    DWORD capacity = 512;
    for (int attempt = 0; attempt < 7; ++attempt) {
        std::vector<wchar_t> buffer(capacity);
        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), capacity);
        if (length == 0) return {};
        if (length < capacity)
            return std::wstring(buffer.data(), length);
        capacity *= 2;
    }
    return {};
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

bool environment_has(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(name, nullptr, 0) != 0;
}

void log_loaded_module(const wchar_t* name, const char* label) {
    const HMODULE module = GetModuleHandleW(name);
    if (!module) {
        flog("[platform] %s module is not loaded", label);
        return;
    }
    const std::string path = utf8(module_path(module));
    if (path.empty()) {
        flog("[platform] %s module is loaded at %p (path unavailable)", label,
             module);
        return;
    }
    flog("[platform] %s module: %s", label, path.c_str());
}

} // namespace

void log_environment() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto wine_get_version = ntdll
        ? reinterpret_cast<WineStringFn>(
              GetProcAddress(ntdll, "wine_get_version"))
        : nullptr;
    const auto wine_get_build_id = ntdll
        ? reinterpret_cast<WineStringFn>(
              GetProcAddress(ntdll, "wine_get_build_id"))
        : nullptr;

    if (wine_get_version) {
        const char* const version = wine_get_version();
        const char* const build = wine_get_build_id ? wine_get_build_id() : nullptr;
        const bool proton_environment =
            environment_has(L"STEAM_COMPAT_DATA_PATH") ||
            environment_has(L"STEAM_COMPAT_CLIENT_INSTALL_PATH");
        flog("[platform] Wine-compatible runtime detected: version=%s, "
             "build=%s, proton_environment=%s",
             version ? version : "unknown", build ? build : "unknown",
             proton_environment ? "yes" : "not advertised");
    } else {
        flog("[platform] native Windows runtime detected");
    }

#if defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
    flog("[platform] MSVC runtime linkage: static (no VC redistributable required)");
#elif defined(_MSC_VER)
    flog("[platform] [WARN] MSVC runtime linkage: dynamic (host VC redistributable required)");
#endif

    // On Proton these paths reveal whether the game is using the expected
    // Wine/vkd3d-proton modules or an unexpected native DLL override.
    log_loaded_module(L"dxgi.dll", "DXGI");
    log_loaded_module(L"d3d12.dll", "D3D12");
    log_loaded_module(L"d3dcompiler_47.dll", "D3DCompiler_47");
}

} // namespace cte::runtime_compat
