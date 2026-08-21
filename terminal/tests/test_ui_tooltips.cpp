// TOOLTIP HYGIENE, audited as source text.
//
// Tooltips are the only documentation this app has at the moment a decision is
// made — the operator is looking at "hold — don't halt" with a live position on
// and needs to know what unticking it does. Nothing constructs an ImGui context
// here, so these scan the source.
//
// The printf rule is the one that is a BUG rather than a style point.
// ImGui::SetItemTooltip is printf-formatted, so a literal percent must be
// written "%%". A single "%" makes ImGui read an argument that was never passed:
// the tooltip renders as garbage, and with a "%s" it dereferences a junk
// pointer. app.cpp already carries "rank by ATR%%" because someone hit this.
#include "doctest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Tooltip {
    std::string file;
    int line = 0;
    std::string text;    // concatenated literal contents
    bool has_args = false;
};

std::string slurp(const std::string& rel) {
    const std::string path = std::string(TT_REPO_DIR) + rel;
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// Every SetTooltip / SetItemTooltip call in a file, with its argument text
// flattened. Deliberately simple: it must agree with what the compiler sees,
// and anything it cannot parse it reports rather than skips.
std::vector<Tooltip> tooltips_in(const std::string& rel) {
    const std::string src = slurp(rel);
    std::vector<Tooltip> out;
    for (size_t at = 0;;) {
        size_t call = std::string::npos;
        for (const char* name : {"SetItemTooltip(", "SetTooltip("}) {
            const size_t p = src.find(name, at);
            if (p != std::string::npos && (call == std::string::npos || p < call))
                call = p;
        }
        if (call == std::string::npos) break;
        const size_t open = src.find('(', call);
        size_t i = open + 1;
        int depth = 1;
        for (; i < src.size() && depth; ++i) {
            if (src[i] == '"') {   // skip string literals, escapes and all
                for (++i; i < src.size() && src[i] != '"'; ++i)
                    if (src[i] == '\\') ++i;
                continue;
            }
            if (src[i] == '(') ++depth;
            else if (src[i] == ')') --depth;
        }
        const std::string arg = src.substr(open + 1, i - open - 2);

        Tooltip t;
        t.file = rel;
        t.line = 1 + static_cast<int>(std::count(src.begin(), src.begin() + call, '\n'));
        for (size_t p = 0; p < arg.size(); ++p) {
            if (arg[p] != '"') continue;
            for (++p; p < arg.size() && arg[p] != '"'; ++p) {
                if (arg[p] == '\\' && p + 1 < arg.size()) {
                    ++p;                          // \n, \" etc: not literal text
                    continue;
                }
                t.text.push_back(arg[p]);
            }
        }
        // A runtime argument after the format string, e.g. SetItemTooltip("%s", tip).
        for (size_t p = arg.rfind('"'); p != std::string::npos && p + 1 < arg.size(); ) {
            const size_t c = arg.find(',', p);
            t.has_args = c != std::string::npos;
            break;
        }
        out.push_back(std::move(t));
        at = i;
    }
    return out;
}

const char* kUiFiles[] = {
    "/terminal/src/app.cpp",
    "/terminal/src/ui_hints.h",
    "/terminal/src/panels/trade.cpp",
    "/terminal/src/panels/sweep.cpp",
    "/terminal/src/panels/replay.cpp",
    "/terminal/src/panels/backtest.cpp",
    "/terminal/src/panels/chart.cpp",
    "/terminal/src/panels/strategy_mgr.cpp",
};

}  // namespace

TEST_CASE("every literal percent in a tooltip is escaped") {
    // THE BUG ONE. An unescaped % makes printf read an argument that does not
    // exist. Skipped only where the call really does pass one.
    size_t seen = 0;
    for (const char* f : kUiFiles) {
        for (const Tooltip& t : tooltips_in(f)) {
            ++seen;
            if (t.has_args) continue;
            for (size_t i = 0; i < t.text.size(); ++i) {
                if (t.text[i] != '%') continue;
                const bool escaped = i + 1 < t.text.size() && t.text[i + 1] == '%';
                INFO(t.file << ":" << t.line << "  " << t.text);
                CHECK_MESSAGE(escaped, "unescaped % in a tooltip - write %% for a "
                                       "literal percent sign");
                if (escaped) ++i;
            }
        }
    }
    // The scanner must actually be finding them; a silent 0 would make every
    // assertion here vacuous, which is the failure mode these audits exist to
    // avoid in the first place.
    CHECK(seen > 30);
}

TEST_CASE("tooltips stay short enough to read in a hover") {
    // Not a style preference: a tooltip long enough to need reading twice gets
    // dismissed, and then the control has no documentation at all. The cap is
    // generous - the legitimately long ones compare three or four options, where
    // a list IS the clearest form - but it catches changelog prose, incident
    // dates and dollar figures drifting in, which is what happened to "Flatten
    // on stop" (it reached ~520 characters before 0.34.5).
    constexpr size_t kMax = 420;
    for (const char* f : kUiFiles) {
        for (const Tooltip& t : tooltips_in(f)) {
            INFO(t.file << ":" << t.line << "  (" << t.text.size() << " chars)  "
                        << t.text);
            CHECK(t.text.size() <= kMax);
        }
    }
}

TEST_CASE("tooltips read as UI text, not as source comments") {
    for (const char* f : kUiFiles) {
        for (const Tooltip& t : tooltips_in(f)) {
            if (t.has_args || t.text.empty()) continue;
            INFO(t.file << ":" << t.line << "  " << t.text);
            // Sentence case: a tooltip starting lowercase reads as a fragment
            // of the code that produced it.
            CHECK_FALSE(static_cast<bool>(std::islower(
                static_cast<unsigned char>(t.text[0]))));
            // No identifiers. These name things the operator cannot see and
            // cannot act on; "on_bar strategies" and "conflated top-of-book"
            // were both real, and both said nothing to the person reading them.
            for (const char* jargon : {"_t ", "()", "cfg_", "->", "::"}) {
                INFO("contains " << jargon);
                CHECK(t.text.find(jargon) == std::string::npos);
            }
        }
    }
}
