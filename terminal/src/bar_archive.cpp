#include "bar_archive.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace tt::ui {

std::string bar_archive_name(const std::string& symbol, const std::string& interval) {
    std::string out;
    out.reserve(symbol.size() + interval.size() + 8);
    const auto append_safe = [&out](const std::string& s) {
        for (const char c : s) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            out.push_back(ok ? c : '_');
        }
    };
    append_safe(symbol);
    out.push_back('_');
    append_safe(interval);
    out += ".csv";
    return out;
}

std::string bar_series_csv(const std::vector<Candle>& candles) {
    std::string out = "ts,open,high,low,close,volume\n";
    // ~56 bytes a row at %.10g on cent-quoted prices; one allocation instead of
    // ten thousand reallocs on a 6-month 5-minute series.
    out.reserve(out.size() + candles.size() * 56);
    char row[192];
    for (const Candle& c : candles) {
        const int n = std::snprintf(row, sizeof row,
                                    "%lld,%.10g,%.10g,%.10g,%.10g,%.10g\n",
                                    static_cast<long long>(c.ts), c.open, c.high,
                                    c.low, c.close, c.volume);
        if (n > 0 && static_cast<size_t>(n) < sizeof row) out.append(row, n);
    }
    return out;
}

bool bar_archive_write(const std::string& dir, const std::string& name,
                       const std::string& csv) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);   // ec ignored: the open below decides
    const std::filesystem::path target = std::filesystem::path(dir) / name;
    const std::filesystem::path tmp = std::filesystem::path(dir) / (name + ".tmp");

    // Write the whole thing, then rename. Anything that fails leaves the
    // previous archive untouched, which is the point of the dance.
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.string().c_str(), "wb") != 0 || !f) return false;
    const size_t want = csv.size();
    const size_t got = want ? std::fwrite(csv.data(), 1, want, f) : 0;
    // fclose's RETURN VALUE, not a separate fflush. An earlier cut called
    // fflush and then ignored fclose, with a comment claiming the flush was what
    // made the rename safe — it was not: fclose flushes too, so the fflush was
    // redundant and the comment was wrong. Mutation testing caught it by
    // deleting the fflush and watching every test still pass.
    //
    // fclose is where a full disk actually surfaces: fwrite can succeed into a
    // buffer that only fails on the way out.
    const bool closed_ok = std::fclose(f) == 0;
    if (!archive_write_ok(got, want, closed_ok)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        // Windows will not rename over an existing file on every filesystem;
        // fall back to replace, and only then give up.
        std::error_code ec2;
        std::filesystem::remove(target, ec2);
        std::filesystem::rename(tmp, target, ec2);
        if (ec2) {
            std::filesystem::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

int bar_archive_prune(const std::string& dir, int days) {
    if (days <= 0) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    const auto max_age = std::chrono::seconds(static_cast<int64_t>(days) * 86'400);
    int removed = 0;
    // NON-recursive, and only our own ".csv" files. This function deletes, so its
    // blast radius is written down rather than implied.
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code fe;
        if (!e.is_regular_file(fe) || fe) continue;
        const std::string fn = e.path().filename().string();
        if (fn.size() < 5 || fn.compare(fn.size() - 4, 4, ".csv") != 0) continue;
        const auto wt = std::filesystem::last_write_time(e.path(), fe);
        if (fe) continue;
        // The file's own age, from its own clock — no clock_cast, which is not
        // available on every toolchain this builds with.
        const auto age = decltype(wt)::clock::now() - wt;
        if (std::chrono::duration_cast<std::chrono::seconds>(age) <= max_age)
            continue;   // young enough: keep
        std::error_code rm;
        if (std::filesystem::remove(e.path(), rm)) ++removed;
    }
    return removed;
}

} // namespace tt::ui
