#pragma once
// Internal to tt_net: JSON/number/timestamp helpers shared by the broker
// and market-data translation units.

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace tt::net_util {

// Some vendors send numbers as JSON strings and others as JSON
// numbers on the data stream — accept both.
inline double num(const nlohmann::json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get_ref<const std::string&>());
        } catch (...) {}
    }
    return 0.0;
}

inline double num_field(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() ? num(*it) : 0.0;
}

// "2026-07-03T18:23:01.123456789-04:00" / "...Z" -> epoch ns, 0 on failure.
// Takes a view and copies to the stack: no allocation on the feed thread.
inline int64_t rfc3339_ns(std::string_view sv) {
    char s[48];
    if (sv.size() >= sizeof s) return 0;
    std::memcpy(s, sv.data(), sv.size());
    s[sv.size()] = '\0';
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
        return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    const std::time_t t = _mkgmtime(&tm);
    if (t == static_cast<std::time_t>(-1)) return 0;

    const size_t n = sv.size();
    size_t i = 19;   // past "YYYY-MM-DDTHH:MM:SS"
    int64_t frac_ns = 0;
    if (i < n && s[i] == '.') {
        int64_t frac = 0, scale = 100'000'000;
        for (++i; i < n && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
            if (scale > 0) {
                frac += (s[i] - '0') * scale;
                scale /= 10;
            }
        }
        frac_ns = frac;
    }
    int64_t off_s = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        int oh = 0, om = 0;
        if (std::sscanf(s + i + 1, "%d:%d", &oh, &om) == 2)
            off_s = (s[i] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return (static_cast<int64_t>(t) - off_s) * 1'000'000'000 + frac_ns;
}

inline size_t curl_write_to_string(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

} // namespace tt::net_util
