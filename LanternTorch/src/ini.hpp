// ============================================================
//  Minimal, dependency-free INI parser.
//  Sections in [brackets], key = value pairs, ';' or '#' comments.
// ============================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

class Ini {
public:
    // Returns false if the file could not be opened.
    bool load(const std::wstring& path) {
        std::ifstream f(path);
        if (!f) return false;

        std::string line, section;
        while (std::getline(f, line)) {
            const std::string s = trim(strip_comment(line));
            if (s.empty()) continue;

            if (s.front() == '[' && s.back() == ']') {
                section = trim(s.substr(1, s.size() - 2));
                continue;
            }

            const auto eq = s.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = trim(s.substr(0, eq));
            const std::string val = trim(s.substr(eq + 1));
            data_[section][key] = val;
        }
        return true;
    }

    std::string get_string(const std::string& sec, const std::string& key,
                           const std::string& def = "") const {
        const auto si = data_.find(sec);
        if (si == data_.end()) return def;
        const auto ki = si->second.find(key);
        return ki == si->second.end() ? def : ki->second;
    }

    bool get_bool(const std::string& sec, const std::string& key, bool def) const {
        const std::string v = lower(get_string(sec, key));
        if (v.empty()) return def;
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    float get_float(const std::string& sec, const std::string& key, float def) const {
        const std::string v = get_string(sec, key);
        if (v.empty()) return def;
        try { return std::stof(v); } catch (...) { return def; }
    }

private:
    static std::string strip_comment(const std::string& s) {
        const size_t p = s.find_first_of(";#");
        return p == std::string::npos ? s : s.substr(0, p);
    }

    static std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> data_;
};
