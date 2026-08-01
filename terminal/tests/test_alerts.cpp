// AlertNotifier's webhook payload must be ASCII: ntfy.sh serves any non-ASCII
// request body as a downloadable "attachment.txt" instead of an inline message,
// so an alert containing an em-dash (RISK HALT / KILL SWITCH / PROTECTIVE STOP
// lines all do) would otherwise reach the phone unreadable. detail::ascii_fold
// collapses each run of non-ASCII bytes to a single '-'.
#include "doctest.h"

#include "alerts.h"

using tt::ui::detail::ascii_fold;

TEST_CASE("ascii_fold: pure-ASCII text is unchanged") {
    const std::string s = "WATCHDOG broker disconnected for 62s - check IB Gateway (2FA?)";
    CHECK(ascii_fold(s) == s);
}

TEST_CASE("ascii_fold: an em-dash becomes a single hyphen") {
    // "—" is U+2014 = 3 UTF-8 bytes; the whole run collapses to one '-'.
    CHECK(ascii_fold("RISK HALT \xE2\x80\x94 flatten") == "RISK HALT - flatten");
}

TEST_CASE("ascii_fold: adjacent non-ASCII bytes collapse to one hyphen") {
    // en-dash then em-dash back to back -> a single '-', not several.
    CHECK(ascii_fold("a\xE2\x80\x93\xE2\x80\x94""b") == "a-b");
}

TEST_CASE("ascii_fold: result is always pure ASCII") {
    const std::string folded = ascii_fold("caf\xC3\xA9 \xE2\x80\x94 na\xC3\xAFve");
    for (unsigned char c : folded) CHECK(c < 0x80);
}

TEST_CASE("ascii_fold: empty stays empty") {
    CHECK(ascii_fold("").empty());
}
