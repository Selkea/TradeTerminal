// Per-symbol parameters: which set a symbol trades, and whether a lineup pick
// is fit to trade at all.
//
// On 2026-08-10 all six live symbols (SNXX, MUU, SOXS, KORU, SOXL, SSPC) ran
// ONE byte-identical parameter set, fitted to SSPC in the 09:40:40 sweep. Four
// of them had lost every tournament candidate to candle-fetch timeouts and
// traded anyway. These cases pin the two rules that make that impossible:
//   - a symbol prefers its OWN values and reports when it inherits, and
//   - a pick with no validated set of its own is dropped, not silently lent
//     another instrument's.
// The config cases pin the persistence half: per-symbol params must round-trip,
// and an old config with "params": {} must still load and still inherit.
#include "doctest.h"

#include "config.h"
#include "symbol_params.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace tt::ui;

// The RSI-2 shape the incident actually ran.
static std::vector<ParamDefault> declared() {
    return {{"rsi_len", 2.0}, {"exit_len", 5.0}, {"risk_pct", 1.0}};
}

// ------------------------------------------------------------- selection

TEST_CASE("params: a symbol with a full set of its own inherits nothing") {
    const std::map<std::string, double> own{
        {"rsi_len", 9.227}, {"exit_len", 12.0}, {"risk_pct", 0.5}};
    const ParamSelection s = select_symbol_params(own, declared());
    CHECK(s.source == ParamSource::Own);
    CHECK(s.own_count == 3);
    CHECK(s.inherited_count == 0);
    CHECK(s.params.at("rsi_len") == doctest::Approx(9.227));
    CHECK(s.inherited_names.empty());
}

TEST_CASE("params: an empty own set is Inherited, not silently Own") {
    // This is the 2026-08-10 state of all six tabs: params == {}. The values are
    // still filled (the engine needs every declared name), but the SOURCE has to
    // come back Inherited or the borrow stays invisible.
    const ParamSelection s = select_symbol_params({}, declared());
    CHECK(s.source == ParamSource::Inherited);
    CHECK(s.own_count == 0);
    CHECK(s.inherited_count == 3);
    CHECK(s.params.size() == 3);
    CHECK(s.params.at("rsi_len") == doctest::Approx(2.0));
    CHECK(s.inherited_names.size() == 3);
}

TEST_CASE("params: a partial own set is Mixed and names what it borrowed") {
    const std::map<std::string, double> own{{"rsi_len", 9.227}};
    const ParamSelection s = select_symbol_params(own, declared());
    CHECK(s.source == ParamSource::Mixed);
    CHECK(s.own_count == 1);
    CHECK(s.inherited_count == 2);
    CHECK(s.params.at("rsi_len") == doctest::Approx(9.227));
    CHECK(s.params.at("exit_len") == doctest::Approx(5.0));
    REQUIRE(s.inherited_names.size() == 2);
    CHECK(s.inherited_names[0] == "exit_len");
    CHECK(s.inherited_names[1] == "risk_pct");
}

TEST_CASE("params: keys the strategy does not declare are dropped") {
    // Left over from a strategy this symbol no longer runs. Passing them on is
    // how a mismatched set reaches the engine, where ctx.param() just returns
    // the fallback and nothing complains (0.9.0 shipped exactly that).
    const std::map<std::string, double> own{{"rsi_len", 9.227}, {"orb_minutes", 30.0}};
    const ParamSelection s = select_symbol_params(own, declared());
    CHECK(s.params.count("orb_minutes") == 0);
    CHECK(s.params.size() == 3);
    CHECK(s.own_count == 1);
}

TEST_CASE("params: a strategy with no declared parameters selects nothing") {
    const ParamSelection s = select_symbol_params({{"stale", 1.0}}, {});
    CHECK(s.source == ParamSource::None);
    CHECK(s.params.empty());
    CHECK(std::string(param_source_name(s.source)) == "none");
}

// ------------------------------------------------------------- admission

TEST_CASE("lineup: a pick whose tournament produced nothing is excluded") {
    // SOXS on 2026-08-10: all five candidates lost to candle-fetch timeouts, and
    // it had no set of its own (set_lineup had cleared the tab).
    SymbolOutcome o;
    o.tournament_ran = true;
    o.produced_result = false;
    o.has_own_params = false;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Exclude);
}

TEST_CASE("lineup: no result but its OWN previous fit keeps it in") {
    // Stale, but fitted to THIS instrument — which another symbol's fit never is.
    SymbolOutcome o;
    o.tournament_ran = true;
    o.produced_result = false;
    o.has_own_params = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::AdmitOwnPrevious);
}

TEST_CASE("lineup: a tournament that produced a result admits the symbol") {
    SymbolOutcome o;
    o.tournament_ran = true;
    o.produced_result = true;
    o.has_own_params = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Admit);
}

TEST_CASE("lineup: a hand-added tab is never judged by the lineup") {
    // No tournament was run for it, so there is no verdict to act on; dropping
    // it would delete a tab the user typed in.
    SymbolOutcome o;
    o.tournament_ran = false;
    o.produced_result = false;
    o.has_own_params = false;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Admit);
}

TEST_CASE("lineup: the 2026-08-10 morning drops four and starts two") {
    auto ran = [](bool result, bool own) {
        SymbolOutcome o;
        o.tournament_ran = true;
        o.produced_result = result;
        o.has_own_params = own;
        return o;
    };
    const LineupPlan p = plan_lineup({{"SNXX", ran(true, true)},
                                      {"MUU", ran(false, false)},
                                      {"SOXS", ran(false, false)},
                                      {"KORU", ran(false, false)},
                                      {"SOXL", ran(false, false)},
                                      {"SSPC", ran(true, true)}});
    CHECK(p.start);
    REQUIRE(p.admitted.size() == 2);
    CHECK(p.admitted[0] == "SNXX");
    CHECK(p.admitted[1] == "SSPC");
    CHECK(p.excluded.size() == 4);
    CHECK(p.own_previous.empty());
}

TEST_CASE("lineup: every pick failing starts nothing at all") {
    // A session where every symbol runs borrowed parameters is worse than no
    // session, and "start whatever is left" would have been exactly that.
    SymbolOutcome dead;
    dead.tournament_ran = true;
    dead.produced_result = false;
    dead.has_own_params = false;
    const LineupPlan p = plan_lineup(
        {{"MUU", dead}, {"SOXS", dead}, {"KORU", dead}, {"SOXL", dead}});
    CHECK_FALSE(p.start);
    CHECK(p.admitted.empty());
    CHECK(p.excluded.size() == 4);
}

TEST_CASE("lineup: a survivor on its own previous fit still starts the session") {
    SymbolOutcome dead, stale;
    dead.tournament_ran = stale.tournament_ran = true;
    dead.produced_result = stale.produced_result = false;
    dead.has_own_params = false;
    stale.has_own_params = true;
    const LineupPlan p = plan_lineup({{"MUU", dead}, {"SOXS", stale}});
    CHECK(p.start);
    REQUIRE(p.admitted.size() == 1);
    CHECK(p.admitted[0] == "SOXS");
    REQUIRE(p.own_previous.size() == 1);
    CHECK(p.own_previous[0] == "SOXS");
    CHECK(p.excluded.size() == 1);
}

// ------------------------------------------------------- config round-trip

namespace {
struct TempCfg {
    std::filesystem::path p;
    explicit TempCfg(const char* name)
        : p(std::filesystem::temp_directory_path() / name) {}
    ~TempCfg() { std::error_code ec; std::filesystem::remove(p, ec); }
    std::string str() const { return p.string(); }
};
} // namespace

TEST_CASE("config: per-symbol params survive a save/load cycle") {
    // The whole fix is worthless if the symbol's own set does not outlive the
    // process — a restart would put it straight back to inheriting.
    TempCfg f("tt_test_symbol_params.json");
    AppConfig c;
    TradeSymbol a;
    a.symbol = "SOXS";
    a.strat_key = "rsi2_pullback.cpp";
    a.params = {{"rsi_len", 9.227}, {"exit_len", 12.0}};
    TradeSymbol b;
    b.symbol = "SSPC";
    b.strat_key = "rsi2_pullback.cpp";
    b.params = {{"rsi_len", 3.5}};
    c.trade_symbols = {a, b};
    c.save(f.str());

    const AppConfig back = AppConfig::load(f.str());
    REQUIRE(back.trade_symbols.size() == 2);
    CHECK(back.trade_symbols[0].symbol == "SOXS");
    CHECK(back.trade_symbols[0].params.at("rsi_len") == doctest::Approx(9.227));
    CHECK(back.trade_symbols[0].params.at("exit_len") == doctest::Approx(12.0));
    // Two symbols on the SAME strategy must not converge on one set — that is
    // the defect, restated as a persistence property.
    CHECK(back.trade_symbols[1].params.at("rsi_len") == doctest::Approx(3.5));
    CHECK(back.trade_symbols[1].params.size() == 1);
}

TEST_CASE("config: an old config with empty params still loads and inherits") {
    // Backward compatibility: every config written before 0.16.0 has
    // "params": {}. It must load, and selection must report Inherited so the
    // symbol picks up the strategy defaults exactly as it does today.
    TempCfg f("tt_test_symbol_params_legacy.json");
    {
        std::FILE* fp = std::fopen(f.str().c_str(), "wb");
        REQUIRE(fp != nullptr);
        const char* legacy =
            R"({"trade_symbols":[{"symbol":"MUU","bar_sec":300,"strat_key":"rsi2_pullback.cpp","params":{}}]})";
        std::fwrite(legacy, 1, std::strlen(legacy), fp);
        std::fclose(fp);
    }
    const AppConfig back = AppConfig::load(f.str());
    REQUIRE(back.trade_symbols.size() == 1);
    CHECK(back.trade_symbols[0].symbol == "MUU");
    CHECK(back.trade_symbols[0].bar_sec == 300);
    CHECK(back.trade_symbols[0].params.empty());
    const ParamSelection s =
        select_symbol_params(back.trade_symbols[0].params, declared());
    CHECK(s.source == ParamSource::Inherited);
    CHECK(s.params.at("rsi_len") == doctest::Approx(2.0));
}

TEST_CASE("config: a trade_symbols entry with no params key at all loads") {
    // Configs older still never wrote the key. Missing must behave as empty,
    // not as a parse failure that silently discards the whole symbol list.
    TempCfg f("tt_test_symbol_params_nokey.json");
    {
        std::FILE* fp = std::fopen(f.str().c_str(), "wb");
        REQUIRE(fp != nullptr);
        const char* legacy = R"({"trade_symbols":[{"symbol":"KORU","bar_sec":60}]})";
        std::fwrite(legacy, 1, std::strlen(legacy), fp);
        std::fclose(fp);
    }
    const AppConfig back = AppConfig::load(f.str());
    REQUIRE(back.trade_symbols.size() == 1);
    CHECK(back.trade_symbols[0].symbol == "KORU");
    CHECK(back.trade_symbols[0].params.empty());
}
