// ============================================================
//  Tiny memory helpers: module base/size, crash-safe reads, and a
//  byte-pattern (AOB) scanner with "??" wildcards.
//  Self-contained -- no external scanning lib needed.
// ============================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mem {

// ---- the eldenring.exe image span --------------------------------------
struct Module {
    uintptr_t base = 0;
    size_t    size = 0;
};

inline Module main_module() {
    Module m{};
    HMODULE h = GetModuleHandleA("eldenring.exe");
    if (!h) h = GetModuleHandleA(nullptr); // fall back to host exe
    if (!h) return m;
    m.base = reinterpret_cast<uintptr_t>(h);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(h);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
    m.size = nt->OptionalHeader.SizeOfImage;
    return m;
}

// ---- crash-safe pointer read (wrong/stale offsets won't crash the game) -
// POD-only, no C++ unwinding inside the __try frame.
template <typename T>
inline bool safe_read(uintptr_t addr, T& out) {
    if (!addr) return false;
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Follow a pointer: read the uintptr_t at `addr`, return it (0 on fault).
inline uintptr_t deref(uintptr_t addr) {
    uintptr_t v = 0;
    return safe_read(addr, v) ? v : 0;
}

// Crash-safe POD write (mirror of safe_read). Game memory is writable where we
// use this (heap SpEffect entries); a fault just reports false.
template <typename T>
inline bool safe_write(uintptr_t addr, const T& value) {
    if (!addr) return false;
    __try {
        *reinterpret_cast<T*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- AOB scan ----------------------------------------------------------
// Pattern like "48 8B 0D ?? ?? ?? ?? 48 85 C9". "??" = wildcard byte.
// Returns the address of the first match, or 0.
inline uintptr_t aob_scan(const Module& m, const std::string& pattern) {
    if (!m.base || !m.size) return 0;

    std::vector<int> bytes; // -1 == wildcard
    std::istringstream ss(pattern);
    std::string tok;
    while (ss >> tok) {
        if (tok == "??" || tok == "?") bytes.push_back(-1);
        else bytes.push_back(static_cast<int>(std::stoul(tok, nullptr, 16)));
    }
    if (bytes.empty()) return 0;

    const auto* data = reinterpret_cast<const uint8_t*>(m.base);
    const size_t n = bytes.size();
    for (size_t i = 0; i + n <= m.size; ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (bytes[j] != -1 && data[i + j] != bytes[j]) { ok = false; break; }
        }
        if (ok) return m.base + i;
    }
    return 0;
}

// Like aob_scan, but verifies the pattern is UNIQUE (mirrors CE's
// AOBScanModuleUnique). Returns the first match and, via `multiple`, whether a
// second match exists -- a non-unique signature means we may have grabbed the
// wrong function and the caller should refuse to use it.
inline uintptr_t aob_scan_unique(const Module& m, const std::string& pattern,
                                 bool* multiple = nullptr) {
    if (multiple) *multiple = false;
    if (!m.base || !m.size) return 0;

    std::vector<int> bytes; // -1 == wildcard
    std::istringstream ss(pattern);
    std::string tok;
    while (ss >> tok) {
        if (tok == "??" || tok == "?") bytes.push_back(-1);
        else bytes.push_back(static_cast<int>(std::stoul(tok, nullptr, 16)));
    }
    if (bytes.empty()) return 0;

    const auto* data = reinterpret_cast<const uint8_t*>(m.base);
    const size_t n = bytes.size();
    uintptr_t first = 0;
    for (size_t i = 0; i + n <= m.size; ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (bytes[j] != -1 && data[i + j] != bytes[j]) { ok = false; break; }
        }
        if (!ok) continue;
        if (!first) { first = m.base + i; }
        else { if (multiple) *multiple = true; break; }
    }
    return first;
}

// Resolve a RIP-relative reference (e.g. `lea/mov reg,[rip+disp32]`).
// `at` points at the start of the instruction; `disp_off` is the byte offset
// of the disp32 within it; `insn_len` is the instruction length.
inline uintptr_t rip_relative(uintptr_t at, int disp_off, int insn_len) {
    int32_t disp = 0;
    if (!safe_read(at + disp_off, disp)) return 0;
    return at + insn_len + disp;
}

} // namespace mem
