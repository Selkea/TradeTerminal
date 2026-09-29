// The write-only bar archive (0.46.0). net/bar_cache.h is memory-only by design,
// so six months of 5-minute bars evaporate with the process — and on 2026-09-28
// that made "where does tp_r's floor actually belong" unanswerable without a TWS
// connection the live app was holding. See bar_archive.h for why the app must
// never read this back.
#include "doctest.h"

#include "bar_archive.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace tt::ui;

namespace {
// Each case gets its own directory. The suite has bitten this project before by
// sharing fixed filenames in the CWD (see [[tt-test-suite-cwd-hazards]]), so the
// name carries the case.
std::string tmpdir(const char* who) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / (std::string("tt_bar_archive_") + who);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
tt::Candle bar(int64_t ts, double o, double h, double l, double c, double v) {
    return tt::Candle{ts, o, h, l, c, v};
}
} // namespace

TEST_CASE("bar archive: the name is one file per (symbol, interval)") {
    CHECK(bar_archive_name("MUU", "5m") == "MUU_5m.csv");
    CHECK(bar_archive_name("SOXL", "1d") == "SOXL_1d.csv");
}

TEST_CASE("bar archive: a symbol cannot write outside the archive directory") {
    // Symbol and interval both come from the feed. A separator or a traversal in
    // either must not escape — this is the only thing standing between a feed
    // string and an arbitrary path.
    CHECK(bar_archive_name("../../etc", "5m") == ".._.._etc_5m.csv");
    CHECK(bar_archive_name("A/B", "5m") == "A_B_5m.csv");
    CHECK(bar_archive_name("A\\B", "5m") == "A_B_5m.csv");   // a real backslash
    CHECK(bar_archive_name("A:B", "5m") == "A_B_5m.csv");
    // ...while ordinary symbols keep the characters they legitimately use.
    CHECK(bar_archive_name("BRK.B", "5m") == "BRK.B_5m.csv");
    CHECK(bar_archive_name("RDS-A", "1h") == "RDS-A_1h.csv");
}

TEST_CASE("bar archive: the CSV round-trips a series exactly") {
    const std::vector<tt::Candle> in = {
        bar(1787751510, 35.07, 35.20, 35.01, 35.12, 1'000'000.0),
        bar(1787751810, 35.12, 35.44, 35.08, 35.40, 2'500'500.0),
    };
    const std::string csv = bar_series_csv(in);
    // Header first, so a harness can key on names rather than column order.
    CHECK(csv.rfind("ts,open,high,low,close,volume\n", 0) == 0);
    CHECK(csv.find("1787751510,35.07,35.2,35.01,35.12,1000000\n") != std::string::npos);
    CHECK(csv.find("1787751810,35.12,35.44,35.08,35.4,2500500\n") != std::string::npos);
    // Exactly one row per bar plus the header — no blank trailing row to trip a
    // naive reader.
    CHECK(std::count(csv.begin(), csv.end(), '\n') == 3);
}

TEST_CASE("bar archive: an empty series is a header and nothing else") {
    CHECK(bar_series_csv({}) == "ts,open,high,low,close,volume\n");
}

TEST_CASE("bar archive: a write lands, and a rewrite REPLACES") {
    const std::string dir = tmpdir("replace");
    CHECK(bar_archive_write(dir, "MUU_5m.csv", bar_series_csv({bar(1, 1, 1, 1, 1, 1)})));
    const std::string one = slurp(dir + "/MUU_5m.csv");
    CHECK(one.find("1,1,1,1,1,1\n") != std::string::npos);

    // A re-delivery carries the WHOLE series, so the file is replaced rather
    // than appended — that is what makes every write idempotent and means no
    // dedup or merge logic exists to get wrong.
    const std::vector<tt::Candle> longer = {bar(1, 1, 1, 1, 1, 1), bar(2, 2, 2, 2, 2, 2)};
    CHECK(bar_archive_write(dir, "MUU_5m.csv", bar_series_csv(longer)));
    const std::string two = slurp(dir + "/MUU_5m.csv");
    CHECK(two.find("2,2,2,2,2,2\n") != std::string::npos);
    CHECK(std::count(two.begin(), two.end(), '\n') == 3);   // not 4: replaced
}

TEST_CASE("bar archive: the write is atomic - no .tmp is left behind") {
    // A torn file is worse than a missing one: a harness reading a truncated
    // series gets a plausible answer from incomplete data.
    const std::string dir = tmpdir("atomic");
    CHECK(bar_archive_write(dir, "X_5m.csv", bar_series_csv({bar(1, 1, 1, 1, 1, 1)})));
    int csvs = 0, tmps = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string fn = e.path().filename().string();
        if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".csv") ++csvs;
        if (fn.find(".tmp") != std::string::npos) ++tmps;
    }
    CHECK(csvs == 1);
    CHECK(tmps == 0);
}

TEST_CASE("bar archive: the archive directory is created on demand") {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "tt_bar_archive_mkdir";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const std::string nested = (base / "bars").string();
    CHECK(bar_archive_write(nested, "Y_1d.csv", "ts\n"));
    CHECK(std::filesystem::exists(nested + "/Y_1d.csv"));
}

TEST_CASE("bar archive: a short write or a failed close is NOT success") {
    // The condition that decides whether a truncated archive gets published.
    // A short write needs a full disk to reproduce, so the decision is extracted
    // rather than left as an inline expression no test could reach — mutation
    // testing found it surviving exactly because of that.
    CHECK(archive_write_ok(100, 100, true));
    CHECK(archive_write_ok(0, 0, true));          // an empty body is legitimate
    CHECK_FALSE(archive_write_ok(99, 100, true)); // short write: one byte missing
    CHECK_FALSE(archive_write_ok(0, 100, true));  // wrote nothing at all
    // fclose failed, which is where a full disk actually surfaces: fwrite can
    // succeed into a buffer that only fails on the way out.
    CHECK_FALSE(archive_write_ok(100, 100, false));
}

TEST_CASE("bar archive: an unwritable target reports failure, not silent loss") {
    // A file sitting where the directory should be: create_directories cannot
    // succeed and the open must fail. The caller has to learn about it.
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "tt_bar_archive_blocked";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    { std::ofstream f(base / "bars"); f << "I am a file, not a directory"; }
    CHECK_FALSE(bar_archive_write((base / "bars").string(), "Z_5m.csv", "ts\n"));
}

TEST_CASE("bar archive: prune removes stale files and keeps fresh ones") {
    const std::string dir = tmpdir("prune");
    CHECK(bar_archive_write(dir, "OLD_5m.csv", "ts\n"));
    CHECK(bar_archive_write(dir, "NEW_5m.csv", "ts\n"));
    // Backdate one by 60 days, through the file's own clock — which is what
    // prune reads. No injected clock, so the seam cannot be a parameter that
    // algebraically cancels out (an earlier cut of this took a `now_sec` that
    // did exactly that).
    const auto now = std::filesystem::last_write_time(dir + "/OLD_5m.csv");
    std::filesystem::last_write_time(dir + "/OLD_5m.csv", now - std::chrono::hours(24 * 60));

    CHECK(bar_archive_prune(dir, 45) == 1);
    CHECK_FALSE(std::filesystem::exists(dir + "/OLD_5m.csv"));
    CHECK(std::filesystem::exists(dir + "/NEW_5m.csv"));
    // Idempotent: nothing stale is left.
    CHECK(bar_archive_prune(dir, 45) == 0);
}

TEST_CASE("bar archive: prune is disabled at 0 and touches nothing else") {
    const std::string dir = tmpdir("prune_scope");
    CHECK(bar_archive_write(dir, "OLD_5m.csv", "ts\n"));
    const auto now = std::filesystem::last_write_time(dir + "/OLD_5m.csv");
    std::filesystem::last_write_time(dir + "/OLD_5m.csv", now - std::chrono::hours(24 * 60));
    // A stale file that is NOT ours, equally old. This function deletes, so its
    // blast radius is pinned rather than assumed.
    { std::ofstream f(dir + "/journal.db"); f << "not mine"; }
    std::filesystem::last_write_time(dir + "/journal.db", now - std::chrono::hours(24 * 60));

    CHECK(bar_archive_prune(dir, 0) == 0);                       // disabled
    CHECK(std::filesystem::exists(dir + "/OLD_5m.csv"));
    CHECK(bar_archive_prune(dir, 45) == 1);                      // only the .csv
    CHECK(std::filesystem::exists(dir + "/journal.db"));
    CHECK(bar_archive_prune("no/such/dir/anywhere", 45) == 0);   // missing dir is fine
}
