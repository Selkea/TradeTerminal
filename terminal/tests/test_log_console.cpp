// LogConsole::slice_since: the cursor the diagnostics /logs endpoint polls with.
// The cursor is the highest id handed out (not next_id_), so an incremental poll
// must not skip the line whose id sits on the previous boundary — the off-by-one
// this pins down.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "panels/log_console.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using tt::ui::LogConsole;

namespace {
void add_n(LogConsole& lc, int from, int to) {
    for (int i = from; i <= to; ++i) lc.add("line" + std::to_string(i));
}
}  // namespace

TEST_CASE("slice_since: empty ring yields no lines and a zero cursor") {
    LogConsole lc;
    const auto s = lc.slice_since(0);
    CHECK(s.lines.empty());
    CHECK(s.next_id == 0);
    CHECK_FALSE(s.dropped);
}

TEST_CASE("slice_since: first poll returns all; caught-up cursor returns none") {
    LogConsole lc;
    add_n(lc, 1, 3);
    const auto s = lc.slice_since(0);
    CHECK(s.lines.size() == 3);
    CHECK(s.next_id == 3);   // highest existing id, not next_id_ (4)
    CHECK_FALSE(s.dropped);

    const auto s2 = lc.slice_since(s.next_id);
    CHECK(s2.lines.empty());
    CHECK(s2.next_id == 3);
    CHECK_FALSE(s2.dropped);
}

TEST_CASE("slice_since: incremental poll returns only new lines, none skipped") {
    LogConsole lc;
    add_n(lc, 1, 5);
    const uint64_t cur = lc.slice_since(0).next_id;   // == 5
    add_n(lc, 6, 8);
    const auto s = lc.slice_since(cur);
    REQUIRE(s.lines.size() == 3);                     // line6 (the boundary) not dropped
    CHECK(s.lines.front().find("line6") != std::string::npos);
    CHECK(s.lines.back().find("line8") != std::string::npos);
    CHECK(s.next_id == 8);
    CHECK_FALSE(s.dropped);
}

TEST_CASE("slice_since: mid-history cursor returns the exact tail after it") {
    LogConsole lc;
    add_n(lc, 1, 10);
    const auto s = lc.slice_since(3);                 // id > 3 => 4..10
    REQUIRE(s.lines.size() == 7);
    CHECK(s.lines.front().find("line4") != std::string::npos);
    CHECK(s.lines.back().find("line10") != std::string::npos);
    CHECK_FALSE(s.dropped);
}

TEST_CASE("slice_since: a window that fell off the 5000-line ring flags dropped") {
    LogConsole lc;
    add_n(lc, 1, 6000);                               // ring retains ids 1001..6000
    const auto s = lc.slice_since(1);
    CHECK(s.dropped);
    CHECK(s.lines.size() == 5000);
    CHECK(s.lines.front().find("line1001") != std::string::npos);
    CHECK(s.next_id == 6000);
}

TEST_CASE("slice_since: cursor ahead of the newest id resyncs to a tail") {
    LogConsole lc;
    add_n(lc, 1, 3);
    const auto s = lc.slice_since(999999);            // stale cursor from a prior process
    CHECK(s.dropped);
    CHECK(s.lines.size() == 3);
    CHECK(s.next_id == 3);
}

// ------------------------------------------------------------------ rotation
//
// set_log_file rotates an oversized log aside on startup. Until 0.20.0 it
// rotated to a FIXED "<path>.1", so every rotation destroyed the previous one —
// and on 2026-08-11 a GUI launched by mistake, while a dry run was in progress,
// rotated the production optimizer.log and took the 11.25 MB 2026-08-10 baseline
// with it. The whole point of these tests is that rotation may only ever ADD.

namespace {
namespace fs = std::filesystem;

// A disposable directory for one test case.
fs::path scratch(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path() /
                 ("tt_logrot_" + std::string(tag) + "_" + std::to_string(stamp));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_bytes(const fs::path& p, size_t n, char fill) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const std::string chunk(64 * 1024, fill);
    for (size_t w = 0; w < n; w += chunk.size())
        f.write(chunk.data(), static_cast<std::streamsize>(std::min(chunk.size(), n - w)));
}

// Rotations of `base`: everything in the directory whose name is base + a
// ".YYYYMMDD-HHMMSS" suffix.
std::vector<std::string> rotations(const fs::path& dir, const std::string& base) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() == base.size() + 16 && n.rfind(base + ".", 0) == 0)
            out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

constexpr size_t kOverLimit = 5 * 1024 * 1024 + 1;   // > LogConsole::kRotateBytes
} // namespace

TEST_CASE("rotation: an oversized log is moved aside, not truncated") {
    const fs::path dir = scratch("basic");
    const fs::path log = dir / "optimizer.log";
    write_bytes(log, kOverLimit, 'A');

    LogConsole lc;
    lc.set_log_file(log.string());
    const auto rot = rotations(dir, "optimizer.log");
    REQUIRE(rot.size() == 1);
    CHECK(fs::file_size(dir / rot[0]) == kOverLimit);
    CHECK_FALSE(fs::exists(log));   // the live file starts fresh on the next add()

    lc.add("first line of the new file");
    CHECK(fs::exists(log));
    fs::remove_all(dir);
}

TEST_CASE("rotation: a second rotation cannot destroy the first") {
    // THE 2026-08-11 DEFECT. Two startups that each find an oversized log used
    // to produce one surviving rotation, because the second rename landed on the
    // same "<path>.1" as the first. The evidence a human was in the middle of
    // reading was the thing deleted.
    const fs::path dir = scratch("twice");
    const fs::path log = dir / "optimizer.log";

    write_bytes(log, kOverLimit, 'A');   // the baseline worth keeping
    LogConsole first;
    first.set_log_file(log.string());
    const auto after_one = rotations(dir, "optimizer.log");
    REQUIRE(after_one.size() == 1);

    // The stamp has one-second resolution, so a genuinely distinct rotation has
    // to be at least a second later — which every real restart is.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    write_bytes(log, kOverLimit, 'B');   // today's log, also oversized
    LogConsole second;
    second.set_log_file(log.string());
    const auto after_two = rotations(dir, "optimizer.log");

    CHECK(after_two.size() == 2);                       // added, not replaced
    CHECK(fs::exists(dir / after_one[0]));              // the baseline survived
    CHECK(fs::file_size(dir / after_one[0]) == kOverLimit);
    fs::remove_all(dir);
}

TEST_CASE("rotation: a log under the limit is left completely alone") {
    const fs::path dir = scratch("small");
    const fs::path log = dir / "app.log";
    write_bytes(log, 1024, 'x');
    LogConsole lc;
    lc.set_log_file(log.string());
    CHECK(fs::exists(log));
    CHECK(fs::file_size(log) == 1024);
    CHECK(rotations(dir, "app.log").empty());
    fs::remove_all(dir);
}

TEST_CASE("rotation: pruning drops the OLDEST and only its own files") {
    // Rotations must be bounded or a 5 MB log becomes unbounded disk. But the
    // prune deletes files, so it must be incapable of reaching anything that is
    // not one of its own stamped rotations — including the pre-0.20.0 ".1" and
    // the dry run's ".err" sidecar, which live in the same directory.
    const fs::path dir = scratch("prune");
    const fs::path log = dir / "app.log";
    std::vector<std::string> planted;
    for (int i = 1; i <= 8; ++i) {
        const std::string n = "app.log.2026080" + std::to_string(i) + "-000000";
        write_bytes(dir / n, 16, static_cast<char>('0' + i));
        planted.push_back(n);
    }
    // Neighbours that are NOT rotations of this log.
    write_bytes(dir / "app.log.1", 16, 'z');       // what the old code produced
    write_bytes(dir / "app.log.err", 16, 'z');     // the dry run's stderr sidecar
    write_bytes(dir / "other.log.20260801-000000", 16, 'z');

    write_bytes(log, kOverLimit, 'A');
    LogConsole lc;
    lc.set_log_file(log.string());

    const auto rot = rotations(dir, "app.log");
    CHECK(rot.size() < planted.size() + 1);            // something was pruned
    CHECK_FALSE(fs::exists(dir / planted.front()));    // the oldest went first
    CHECK(fs::exists(dir / planted.back()));           // the newest stayed
    // Bystanders untouched.
    CHECK(fs::exists(dir / "app.log.1"));
    CHECK(fs::exists(dir / "app.log.err"));
    CHECK(fs::exists(dir / "other.log.20260801-000000"));
    fs::remove_all(dir);
}
