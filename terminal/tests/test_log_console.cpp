// LogConsole::slice_since: the cursor the diagnostics /logs endpoint polls with.
// The cursor is the highest id handed out (not next_id_), so an incremental poll
// must not skip the line whose id sits on the previous boundary — the off-by-one
// this pins down.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "panels/log_console.h"

#include <string>

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
