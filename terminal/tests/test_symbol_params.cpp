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
#include <fstream>
#include <iterator>
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
    o.fitted_this_build = false;
    o.has_own_params = false;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Exclude);
}

TEST_CASE("lineup: every way a tournament can end without a fit excludes") {
    // The rule is driven by a POSITIVE record of "this build installed a fit on
    // this tab", so it does not need to enumerate failures — but these are the
    // ones the 0.16.0 draft's "not in the failed list" inference read as
    // SUCCESS, because only one of them ever appended to that list:
    //   - every candidate's candle fetch timed out   (the only one it caught)
    //   - the champion scored <= 0 and was rejected, so nothing was installed
    //   - start_tournament bailed: engine busy / every strategy excluded
    //   - "engine stayed busy, aborting" never reached finish_tournament
    //   - the operator hit Cancel in the Optimizer panel
    // All five leave fitted_this_build false, which is the whole point.
    SymbolOutcome o;
    o.tournament_ran = true;
    o.fitted_this_build = false;
    o.has_own_params = false;
    o.holds_position = false;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Exclude);
}

TEST_CASE("lineup: a symbol we still hold is kept even with nothing of its own") {
    // Exclusion deletes the tab, and for an exposed symbol that means either
    // market-closing the position (begin_lineup_swap flattens every dropped
    // symbol, bypassing hold-until-profitable) or orphaning it outside
    // cfg.symbols where reconciliation, /diag, the orphan watchdog and the EOD
    // backstop all miss it. It stays, to be adopted and held until flat.
    SymbolOutcome o;
    o.tournament_ran = true;
    o.fitted_this_build = false;
    o.has_own_params = false;
    o.holds_position = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::AdmitHoldingOnly);

    const LineupPlan p = plan_lineup({{"KORU", o}});
    CHECK(p.start);
    CHECK(p.excluded.empty());
    REQUIRE(p.admitted.size() == 1);
    CHECK(p.admitted[0] == "KORU");
    REQUIRE(p.holding_only.size() == 1);
    CHECK(p.holding_only[0] == "KORU");
}

TEST_CASE("lineup: its own previous fit outranks the holding exemption") {
    // Holding-only is the last resort: a symbol with a fit of its own trades on
    // it normally, position or not.
    SymbolOutcome o;
    o.tournament_ran = true;
    o.fitted_this_build = false;
    o.has_own_params = true;
    o.holds_position = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::AdmitOwnPrevious);
}

TEST_CASE("lineup: no result but its OWN previous fit keeps it in") {
    // Stale, but fitted to THIS instrument — which another symbol's fit never is.
    SymbolOutcome o;
    o.tournament_ran = true;
    o.fitted_this_build = false;
    o.has_own_params = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::AdmitOwnPrevious);
}

TEST_CASE("lineup: a tournament that produced a result admits the symbol") {
    SymbolOutcome o;
    o.tournament_ran = true;
    o.fitted_this_build = true;
    o.has_own_params = true;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Admit);
}

TEST_CASE("lineup: a hand-added tab is never judged by the lineup") {
    // No tournament was run for it, so there is no verdict to act on; dropping
    // it would delete a tab the user typed in.
    SymbolOutcome o;
    o.tournament_ran = false;
    o.fitted_this_build = false;
    o.has_own_params = false;
    CHECK(admit_lineup_symbol(o) == LineupAdmit::Admit);
}

TEST_CASE("lineup: the 2026-08-10 morning drops four and starts two") {
    auto ran = [](bool result, bool own) {
        SymbolOutcome o;
        o.tournament_ran = true;
        o.fitted_this_build = result;
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
    dead.fitted_this_build = false;
    dead.has_own_params = false;
    const LineupPlan p = plan_lineup(
        {{"MUU", dead}, {"SOXS", dead}, {"KORU", dead}, {"SOXL", dead}});
    CHECK_FALSE(p.start);
    CHECK(p.admitted.empty());
    CHECK(p.excluded.size() == 4);
}

TEST_CASE("lineup: an open position alone is enough to start the session") {
    // start=false means the caller starts NOTHING — which for a symbol we are
    // holding would leave the position with no session watching it. One
    // holding-only admission has to be enough to bring the session up.
    SymbolOutcome dead, held;
    dead.tournament_ran = held.tournament_ran = true;
    held.holds_position = true;
    const LineupPlan p = plan_lineup({{"MUU", dead}, {"SOXL", held}});
    CHECK(p.start);
    REQUIRE(p.admitted.size() == 1);
    CHECK(p.admitted[0] == "SOXL");
    REQUIRE(p.excluded.size() == 1);
    CHECK(p.excluded[0] == "MUU");
}

TEST_CASE("lineup: a survivor on its own previous fit still starts the session") {
    SymbolOutcome dead, stale;
    dead.tournament_ran = stale.tournament_ran = true;
    dead.fitted_this_build = stale.fitted_this_build = false;
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

TEST_CASE("config: a refused trading day survives a restart") {
    // The Trade panel's auto-start is level-triggered anywhere inside the
    // session window, and the VPS relaunches unattended. If the lineup's block
    // lived only in memory, any restart between 09:25 and 15:55 would start
    // exactly the tabs it refused, on the parameters it refused them for.
    TempCfg f("tt_test_sched_block.json");
    AppConfig c;
    c.trade_sched_on = true;
    c.trade_sched_blocked_day = 221;
    c.save(f.str());
    CHECK(AppConfig::load(f.str()).trade_sched_blocked_day == 221);
    // Default is "not blocked", so a config that never saw a refusal starts
    // normally rather than sitting out every day.
    CHECK(AppConfig().trade_sched_blocked_day == -1);
}

TEST_CASE("config: the 0.16.1 strategy-param purge runs once and only once") {
    // Every build up to 0.16.0 let each tournament CANDIDATE write the shared
    // per-strategy map, so an existing install's saved values are fits on
    // whatever symbol was optimized last. A config with no marker must report
    // "not purged" (so the upgrade fires); once written, it must stay true (so
    // deliberate values set afterwards are never discarded again).
    TempCfg f("tt_test_param_purge.json");
    {
        std::FILE* fp = std::fopen(f.str().c_str(), "wb");
        REQUIRE(fp != nullptr);
        const char* legacy = R"({"strategy_params":{"orb.cpp":{"risk_pct":0.05}}})";
        std::fwrite(legacy, 1, std::strlen(legacy), fp);
        std::fclose(fp);
    }
    AppConfig old = AppConfig::load(f.str());
    CHECK_FALSE(old.strategy_params_purged);
    CHECK(old.strategy_params.at("orb.cpp").at("risk_pct") == doctest::Approx(0.05));

    old.strategy_params.clear();
    old.strategy_params_purged = true;
    old.save(f.str());
    CHECK(AppConfig::load(f.str()).strategy_params_purged);
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

// ---------------------------------------------------------------------------
// A TIME STOP THAT CANNOT FIRE INSIDE A TRADING DAY.
//
// 2026-08-14, -$506.26 on STKH: 85% of the day's loss from one trade whose only
// risk control was arithmetically incapable of running. bollinger_reversion
// places NO price stop by design and its z-recovery exit is gated on the price
// being back above the entry, so on a LOSING trade the time stop is the only
// exit that exists. That morning's tournament fitted it to 233 bars of 300 s —
// 19.4 hours, about three trading days — against a session the engine force-
// flattens at 15:57. The position filled at 13:41, held 27 bars, fell 10.1%,
// and was liquidated by the backstop.
//
// Two symbols of five carried an unreachable stop that day (STKH 233, SNDQ
// 375). It is a property of the fit, not of STKH.

TEST_CASE("session_bars: the day, in bars, at the size actually traded") {
    // 09:30 -> 15:57 is 6 h 27 m = 23220 s. The 15:57 flatten, NOT the 16:00
    // close: a position that cannot be closed is not held, and 15:57 is the last
    // moment anything closes one.
    CHECK(session_bars(300) == 77);      // 5-minute bars, the production size
    CHECK(session_bars(60) == 387);
    CHECK(session_bars(3600) == 6);
    // Never "bars left from now". A 09:35 lineup build and a 14:00 rebuild must
    // reach the same verdict about the same numbers, or the gate would depend on
    // when someone happened to press the button.
    CHECK(session_bars(300) == session_bars(300));
    CHECK(session_bars(0) == 0);         // unknown bar size: nothing to say
    CHECK(session_bars(-5) == 0);
}

TEST_CASE("time_stop_reachable: the exact 2026-08-14 numbers") {
    CHECK_FALSE(time_stop_reachable({{"time_stop", 233}}, 300));   // STKH, live
    CHECK_FALSE(time_stop_reachable({{"time_stop", 375}}, 300));   // SNDQ, never entered
    CHECK(time_stop_reachable({{"time_stop", 46}}, 300));          // SNDU, fine
    // The boundary, both sides. 77 bars is exactly one session and IS reachable;
    // 78 is not.
    CHECK(time_stop_reachable({{"time_stop", 77}}, 300));
    CHECK_FALSE(time_stop_reachable({{"time_stop", 78}}, 300));
    // 233 bars is a perfectly ordinary swing hold on DAILY bars. The bound is a
    // fact about the intraday session, so it must not follow the number around.
    CHECK(time_stop_reachable({{"time_stop", 233}}, 86400));
    // Strategies with a real price stop declare no time_stop at all; a 0 is
    // repaired by the strategy itself (bollinger_reversion restores 12).
    CHECK(time_stop_reachable({{"length", 31}, {"entry_z", 1.16}}, 300));
    CHECK(time_stop_reachable({{"time_stop", 0}}, 300));
    // "hold - don't halt" rides positions past the day on purpose. Asking the
    // question there would refuse a configuration that is working as intended.
    CHECK(time_stop_reachable({{"time_stop", 233}}, 300, /*hold_dont_halt=*/true));
    // Unknown bar size: not ours to judge rather than refuse-by-default.
    CHECK(time_stop_reachable({{"time_stop", 233}}, 0));
}

TEST_CASE("admission: an unreachable time stop keeps a symbol out of the lineup") {
    SymbolOutcome stkh;                     // exactly the 2026-08-14 state:
    stkh.tournament_ran = true;             // a tournament ran,
    stkh.fitted_this_build = true;          // it crowned a champion,
    stkh.has_own_params = true;             // the tab carries that champion,
    stkh.time_stop_reachable = false;       // and the champion cannot exit a loser.
    CHECK(admit_lineup_symbol(stkh) == LineupAdmit::ExcludeUnreachableStop);

    // A SEPARATE verdict from Exclude, and that distinction is the point: these
    // two say opposite things about the fit ("nothing was fitted to it" versus
    // "something was, and it is unrealizable") and need different operator
    // lines. Folding them together would send someone hunting a tournament
    // failure that did not happen.
    SymbolOutcome nofit;
    nofit.tournament_ran = true;
    CHECK(admit_lineup_symbol(nofit) == LineupAdmit::Exclude);

    // It outranks the hand-added bypass. "Not ours to judge" is the right answer
    // to "did a tournament validate this symbol"; it is not the right answer to
    // "can anything here close the position it is about to open".
    SymbolOutcome manual;
    manual.tournament_ran = false;
    CHECK(admit_lineup_symbol(manual) == LineupAdmit::Admit);
    manual.time_stop_reachable = false;
    CHECK(admit_lineup_symbol(manual) == LineupAdmit::ExcludeUnreachableStop);

    // ...but NOT over an open position. Dropping an exposed symbol either
    // market-closes it or orphans it outside cfg.symbols, where reconciliation
    // cannot adopt it and the 15:57 backstop cannot flatten it. Refusing to
    // trade a symbol must never create the orphan.
    SymbolOutcome held = stkh;
    held.holds_position = true;
    CHECK(admit_lineup_symbol(held) == LineupAdmit::Admit);

    // Default true, so a caller that has not been taught this question yet
    // cannot silently exclude the entire lineup.
    CHECK(SymbolOutcome{}.time_stop_reachable);
}

TEST_CASE("plan_lineup: unreachable stops are excluded AND named separately") {
    SymbolOutcome ok;
    ok.tournament_ran = ok.fitted_this_build = ok.has_own_params = true;
    SymbolOutcome bad = ok;
    bad.time_stop_reachable = false;
    SymbolOutcome nofit;
    nofit.tournament_ran = true;

    const LineupPlan p = plan_lineup({{"MUU", ok}, {"STKH", bad}, {"SNXX", nofit}});
    CHECK(p.admitted == std::vector<std::string>{"MUU"});
    // In `excluded` as well, because that is the list the tabs are removed by —
    // a symbol reported only in the new bucket would be excluded in the log and
    // still trading in the panel.
    CHECK(p.excluded == std::vector<std::string>{"STKH", "SNXX"});
    CHECK(p.unreachable_stop == std::vector<std::string>{"STKH"});
    CHECK(p.start);

    // Every pick unreachable = start nothing, through the SAME path a lineup of
    // borrowed parameters takes. A session where no symbol can close a losing
    // position is worse than no session.
    const LineupPlan none = plan_lineup({{"STKH", bad}, {"SNDQ", bad}});
    CHECK_FALSE(none.start);
    CHECK(none.unreachable_stop.size() == 2);
}

// ---------------------------------------------------------------------------
// 0.34.1: ALL THREE GATES MUST ASK THE SAME QUESTION.
//
// time_stop_reachable itself is well covered above. What was wrong was an
// ARGUMENT LIST at one of its three call sites, and no unit test can see that —
// nothing in the suite constructs an App. Audited as text, which is weaker than
// running it and is the strongest thing available here.

TEST_CASE("the autopilot judges a fit the way the other two gates do") {
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};

    // THE DEFECT: the autopilot was the only site measuring against the GLOBAL
    // bar size, which is the LAST tab's (trade.cpp def_bar_sec_), and the only
    // one dropping the hold-until-flat exemption. A "hold — don't halt" symbol
    // is exempt at both other gates and may legitimately carry a long time stop,
    // and coordinate descent RETAINS the incumbent's value when no swept point
    // wins — so this gate re-proposed and re-rejected the same legal number
    // every cycle, forever, and the condition could never clear.
    CHECK(src.find("time_stop_reachable(champ->params, cfg_.trade_bar_sec)") ==
          std::string::npos);
    CHECK(src.find("time_stop_reachable(champ->params, bar_sec, tp.found && tp.hold_dont_halt)") !=
          std::string::npos);
    // The bar size it judges against is the SYMBOL's, with the global only as a
    // fallback when the tab is gone.
    CHECK(src.find("tp.found && tp.bar_seconds > 0 ? tp.bar_seconds : cfg_.trade_bar_sec") !=
          std::string::npos);

    // ...and the other two still pass both, so a future edit cannot "fix" the
    // asymmetry by levelling DOWN to the broken call.
    CHECK(src.find("time_stop_reachable(tp.params, tp.bar_seconds, tp.hold_dont_halt)") !=
          std::string::npos);
    CHECK(src.find("time_stop_reachable(so.params, so.bar_seconds,") != std::string::npos);
    CHECK(src.find("so.risk.disable_auto_halt)") != std::string::npos);

    // Every reachability verdict in app.cpp reports the bar size it actually
    // judged against. Printing session_bars(cfg_.trade_bar_sec) beside a verdict
    // reached with tp.bar_seconds would be a line that contradicts itself.
    CHECK(src.find("session_bars(bar_sec)") != std::string::npos);
    // ...and it says so out loud. The figure is bar-size-relative, and now that
    // the gate reads each SYMBOL's bar size rather than one global, a bare "only
    // 77 fit in a trading day" is a number the operator cannot check against
    // anything — two symbols can legitimately print different ones on the same
    // day. Pinned because a wording revert is invisible to every other test here.
    CHECK(src.find("\" fit in a trading day at this bar size)") != std::string::npos);
}

TEST_CASE("the two quiet verdicts are the ones where nothing is exposed") {
    const std::string path = std::string(TT_REPO_DIR) + "/terminal/src/app.cpp";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string src{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    // Declined a challenger (incumbent still running) and refused symbols out of
    // the session: the guard ACTED in both, so both carry a nested tag.
    CHECK(src.find("kUnreachableStopDeclinedTag") != std::string::npos);
    CHECK(src.find("kUnreachableStopRefusedTag") != std::string::npos);
    // The two where the hazard is LIVE keep the bare tag and keep Critical:
    // kept-exposed, and no-symbol-survived.
    CHECK(src.find("kUnreachableStopTag + \" on \" + list +") != std::string::npos);
    CHECK(src.find("kUnreachableStopTag + \". Refit them and start again.\"") !=
          std::string::npos);
    // The old wording must not survive anywhere: it carried the bare tag on a
    // success path, which is the whole defect.
    CHECK(src.find("champion REJECTED - \" +") == std::string::npos);
}
