#include "app.h"
#include "dev_paths.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout) + private dock node flags
#include "implot.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA (gateway login page / launch)
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

namespace tt::ui {

namespace {
std::filesystem::path data_dir() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(base ? base : ".") / "TradeTerminal";
}
std::string strategies_out_dir() { return (data_dir() / "strategies").string(); }
std::string sessions_dir() { return (data_dir() / "sessions").string(); }
std::string gxx_path() {
    const char* env = std::getenv("TT_GXX");
    return env ? env : TT_GXX_DEFAULT;
}

// PowerShell arguments to run the IBeam auto-login helper, or "" if the script
// is missing. Launches a headless browser login against the running gateway
// using credentials saved (DPAPI-encrypted) by scripts\Save-IbkrCred.ps1.
std::string ps_script_path(const char* name) {
    std::error_code ec;
    auto script = std::filesystem::path(TT_REPO_ROOT) / "scripts" / name;
    if (std::filesystem::exists(script, ec)) return script.make_preferred().string();
    return {};
}
// PowerShell argument string for one of the repo scripts, or "" if missing.
std::string ps_args(const char* name, bool hidden = true, const std::string& extra = {}) {
    const std::string p = ps_script_path(name);
    if (p.empty()) return {};
    std::string a = "-NoProfile ";
    if (hidden) a += "-WindowStyle Hidden ";
    a += "-ExecutionPolicy Bypass -File \"" + p + "\"";
    if (!extra.empty()) a += " " + extra;
    return a;
}
std::string signout_args() { return ps_args("Stop-IbkrLogin.ps1"); }
std::string switch_args(const std::string& acct) {
    return ps_args("Switch-IbkrAccount.ps1", true, "-Account \"" + acct + "\"");
}
std::string addnew_args() { return ps_args("Save-IbkrCred.ps1", /*hidden=*/false); }

// Labels + paper flag + active label from the multi-account IBKR credential
// store. Labels/paper are plaintext; usernames/passwords are DPAPI-encrypted.
struct IbkrAccount {
    std::string name;        // unique key: switch/remove act on this
    std::string label;       // display name (defaults to name)
    bool paper = true;
    bool readonly = false;   // live login for viewing/testing; block all orders
};
struct IbkrAccountList {
    std::string active;
    std::vector<IbkrAccount> accounts;

    // Is the active account flagged read-only?
    bool active_readonly() const {
        for (const auto& a : accounts)
            if (a.name == active) return a.readonly;
        return false;
    }
};
IbkrAccountList read_ibkr_accounts() {
    IbkrAccountList r;
    std::ifstream f(data_dir() / "ibkr-accounts.json");
    if (!f) return r;
    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return r;
    r.active = j.value("active", std::string());
    for (const auto& a : j.value("accounts", nlohmann::json::array())) {
        auto nm = a.value("name", std::string());
        if (nm.empty()) continue;
        IbkrAccount acc;
        acc.label = a.value("label", nm);   // display name; defaults to the key
        acc.name = std::move(nm);
        acc.paper = a.value("paper", true);
        acc.readonly = a.value("readonly", false);
        r.accounts.push_back(std::move(acc));
    }
    return r;
}

#ifdef _WIN32
// Launch "powershell.exe <args>" with no console window at all. ShellExecute's
// SW_HIDE can still flash a console for console-subsystem apps; CREATE_NO_WINDOW
// does not, and child processes inherit the no-window state. If wait is set,
// block (up to 12s) for it to finish — used for cleanup on shutdown.
void run_hidden(const std::string& args, bool wait = false) {
    std::string cmd = "powershell.exe " + args;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (wait) WaitForSingleObject(pi.hProcess, 12000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif

// Colored "PAPER" (green) / "LIVE" (red) badge on the current line.
void badge_paper_live(bool paper) {
    ImGui::SameLine();
    if (paper)
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "PAPER");
    else
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1), "LIVE");
}
void badge_kind(net::GatewayData::AccountKind k) {
    using K = net::GatewayData::AccountKind;
    if (k == K::Paper) badge_paper_live(true);
    else if (k == K::Live) badge_paper_live(false);
    // Unknown: no badge
}
} // namespace

std::string App::polygon_key() const {
    if (auto a = accounts_.active("polygon")) return a->key_id;
    const char* k = std::getenv("POLYGON_API_KEY");
    return k && *k ? k : "";
}

std::string App::finnhub_key() const {
    if (auto a = accounts_.active("finnhub")) return a->key_id;
    const char* k = std::getenv("FINNHUB_API_KEY");
    return k && *k ? k : "";
}

App::App(std::string gateway_url)
    : gw_(std::move(gateway_url)),
      host_(gxx_path(), std::string(TT_REPO_ROOT) + "/sdk/include", strategies_out_dir()),
      chart_(gw_, series_),
      watchlist_(gw_, quotes_),
      backtest_(engine_),
      replay_(engine_, sessions_dir()),
      strat_mgr_(host_, engine_, std::string(TT_REPO_ROOT) + "/strategies"),
      trade_(engine_),
      blotter_(engine_),
      positions_(engine_),
      sweep_panel_(engine_),
      accounts_((data_dir() / "accounts.json").string()) {
    net::GatewayData::Callbacks cbs;
    cbs.on_log = [this](std::string line) { log_.add(std::move(line)); };
    cbs.on_tick = [this](const std::string& sym, const Quote& q) {
        quotes_.set(sym, q);
        // When the real-time feed owns the session, delayed sidecar quotes
        // still update the watchlist but must not reach the engine.
        if (engine_.live_running() && !rt_feed_active_.load(std::memory_order_relaxed))
            engine_.push_live_tick(sym, q.ts_ms, q.price, q.day_volume);
    };
    cbs.on_error = [this](uint32_t id, std::string code, std::string msg) {
        log_.add("feed error (req " + std::to_string(id) + ") " + code + ": " + msg);
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_.active = false;  // data never arrives; don't wedge the panel
    };
    cbs.on_candles = [this](net::CandleBatch&& b) {
        log_.add("candles: " + b.symbol + " " + b.interval + " x" +
                 std::to_string(b.candles.size()) + (b.cached ? " (cache)" : ""));
        start_pending_backtest(b);
        stash_pending_sweep(b);
        series_.put(b.symbol, b.interval, std::move(b.candles), b.cached);
    };

    // Session persistence + file logging.
    log_.set_log_file((data_dir() / "logs" / "terminal.log").string());
    config_path_ = (data_dir() / "config.json").string();
    cfg_ = AppConfig::load(config_path_);
    watchlist_.set_symbols(cfg_.watchlist);
    chart_.restore(cfg_.chart_symbol, cfg_.chart_interval_idx, cfg_.chart_range_idx);
    backtest_.set_cash(cfg_.backtest_cash);
    trade_.restore(cfg_.trade_cash, cfg_.trade_bar_sec, cfg_.trade_data_idx,
                   cfg_.trade_record);
    {
        RiskLimits r;
        r.max_order_qty = cfg_.risk_max_order_qty;
        r.max_position_qty = cfg_.risk_max_position_qty;
        r.daily_max_loss = cfg_.risk_daily_max_loss;
        r.stale_feed_sec = cfg_.risk_stale_feed_sec;
        trade_.restore_risk(r, cfg_.risk_max_drawdown_pct);
    }
    trade_.restore_symbols(cfg_.trade_symbols);
    backtest_.set_strategy(cfg_.backtest_strategy);
    // Rebuild last session's strategies and their params.
    strat_mgr_.restore_state(cfg_.strategy_loaded, cfg_.strategy_params);
    const char* wh = std::getenv("TT_ALERT_WEBHOOK");
    alerts_.set_webhook(wh && *wh ? wh : cfg_.alert_webhook);
    if (alerts_.has_webhook()) log_.add("alerts: webhook configured");

    if (!journal_.open((data_dir() / "journal.db").string()))
        log_.add("journal: could not open journal.db — history disabled");

#ifdef TT_DEBUG
    sim_ticks_ = std::getenv("TT_SIM_TICKS") != nullptr;
#endif

    gw_.start(std::move(cbs));

#ifdef _WIN32
    // Tie the IBKR gateway + auto-login daemon to this app's lifetime: start it
    // now (if an account is configured) and stop it in the destructor.
    {
        const auto ib = read_ibkr_accounts();
        if (!ib.accounts.empty() && !ib.active.empty()) {
            const std::string args = ps_args("Start-IbkrLogin.ps1", true, "-Daemon");
            if (!args.empty()) {
                run_hidden(args);
                gateway_starting_until_ = 90.0;   // show INITIALIZING until it connects
                log_.add("account: starting IBKR gateway + auto-login (tied to app)");
            }
        }
    }
#endif
}

// IPC thread: if this candle batch is the one a queued backtest is waiting
// for, convert it and launch the engine. The lock is held through the engine
// start so the UI thread's lease GC can never observe "pending consumed +
// engine idle" while the leased instance is being handed to the engine.
void App::start_pending_backtest(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    if (!pending_bt_.active || pending_bt_.symbol != batch.symbol ||
        pending_bt_.interval != batch.interval)
        return;
    pending_bt_.active = false;   // GC-safe: engine start happens under the lock
    if (batch.candles.size() < 3) {
        log_.add("backtest: not enough data for " + batch.symbol);
        return;
    }
    BacktestConfig cfg;
    cfg.symbol = batch.symbol;
    cfg.bars.reserve(batch.candles.size());
    for (const Candle& c : batch.candles)
        cfg.bars.push_back(Bar{c.ts * 1'000'000'000, c.open, c.high, c.low,
                               c.close, c.volume});
    cfg.initial_cash = pending_bt_.cash;
    cfg.params = std::move(pending_bt_.params);
    if (!engine_.start_backtest(std::move(cfg), pending_bt_.strategy))
        log_.add("backtest: engine busy, try again");
}

// UI thread: lease a fresh instance of `key`, capture params, fetch data.
void App::queue_backtest(const std::string& key, const std::string& sym,
                         const std::string& ivl, const std::string& rng,
                         double cash) {
    if (!gw_.connected()) {
        log_.add("backtest: feed is down, cannot fetch data");
        return;
    }
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        log_.add("backtest: strategy '" + key + "' is not loaded");
        return;
    }
    leases_.push_back({inst, key, StrategyLease::Backtest});
    {
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_ = {true, sym, ivl, strat_mgr_.param_values(key), cash, inst};
    }
    gw_.request_candles(sym, ivl, rng);
}

// "" = a fresh built-in SMA; otherwise an instance from the module's factory.
IStrategy* App::acquire_strategy(const std::string& key) {
    if (key.empty()) return new SmaCrossover();
    return host_.create_instance(key);
}

void App::release_strategy(const StrategyLease& lease) {
    if (lease.key.empty())
        lease.inst->destroy();
    else
        host_.destroy_instance(lease.inst);
}

// UI thread, per frame: destroy leased instances whose run can no longer
// touch them. Conditions are conservative — a lease outliving its run by a
// few frames is fine; destroying early is the segfault class this exists to
// prevent.
void App::pump_leases() {
    for (size_t i = 0; i < leases_.size();) {
        const StrategyLease& l = leases_[i];
        bool done = false;
        switch (l.kind) {
        case StrategyLease::Live:
            done = !engine_.live_running();
            break;
        case StrategyLease::Backtest: {
            bool referenced;
            {
                std::lock_guard lock(pending_bt_mu_);
                referenced = pending_bt_.active && pending_bt_.strategy == l.inst;
            }
            done = !referenced && !engine_.running();
            break;
        }
        case StrategyLease::Sweep: {
            bool referenced;
            {
                std::lock_guard lock(pending_bt_mu_);
                referenced = (sweep_setup_.waiting || sweep_setup_.ready) &&
                             sweep_setup_.strategy == l.inst;
            }
            const bool sweep_active =
                sweep_strategy_ == l.inst && (sweep_.running || sweep_holdout_phase_);
            done = !referenced && !sweep_active && !engine_.running();
            break;
        }
        }
        if (!done) {
            ++i;
            continue;
        }
        if (sweep_strategy_ == l.inst) sweep_strategy_ = nullptr;
        release_strategy(l);
        leases_[i] = leases_.back();
        leases_.pop_back();
    }
}

// UI thread: run with a specific strategy source, building + loading its
// module first when it's absent or stale ("" = built-in, never builds).
void App::queue_backtest_as(const std::string& src, const std::string& sym,
                            const std::string& ivl, const std::string& rng,
                            double cash) {
    if (strat_mgr_.loaded_fresh(src)) {
        queue_backtest(src, sym, ivl, rng, cash);
        return;
    }
    pending_run_ = {true, src, sym, ivl, rng, cash};
    strat_mgr_.request_load(src);
    log_.add("backtest: building " + src + " first");
}

// UI thread, per frame: fire the deferred backtest once the module is
// loaded, or drop it when the build/load failed.
void App::pump_pending_run() {
    if (!pending_run_.active) return;
    if (strat_mgr_.loaded_fresh(pending_run_.src)) {
        pending_run_.active = false;
        queue_backtest(pending_run_.src, pending_run_.symbol,
                       pending_run_.interval, pending_run_.range,
                       pending_run_.cash);
    } else if (!strat_mgr_.load_pending()) {
        pending_run_.active = false;
        log_.add("backtest: strategy build failed — see the Strategy panel");
    }
}

// ------------------------------------------------------------------ sweep

// UI thread: capture the request + strategy and fetch the data.
void App::queue_sweep(const SweepPanel::Request& rq) {
    if (!gw_.connected()) {
        log_.add("sweep: feed is down, cannot fetch data");
        return;
    }
    if (sweep_.running || engine_.running()) {
        log_.add("sweep: engine busy, try again");
        return;
    }
    const std::string key = rq.strat_key;
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        log_.add("sweep: strategy '" + strat_mgr_.display_name(key) + "' is not loaded");
        return;
    }
    leases_.push_back({inst, key, StrategyLease::Sweep});
    {
        std::lock_guard lock(pending_bt_mu_);
        sweep_setup_ = SweepSetup{};
        sweep_setup_.waiting = true;
        sweep_setup_.req = rq;
        sweep_setup_.strategy = inst;
        sweep_setup_.key = key;
        sweep_setup_.params = strat_mgr_.param_values(key);
    }
    gw_.request_candles(rq.symbol, rq.interval, rq.range);
}

// IPC thread: if this batch is what the sweep is waiting for, stash the
// bars; the UI thread picks them up in pump_sweep().
void App::stash_pending_sweep(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    if (!sweep_setup_.waiting || sweep_setup_.req.symbol != batch.symbol ||
        sweep_setup_.req.interval != batch.interval)
        return;
    sweep_setup_.bars.clear();
    sweep_setup_.bars.reserve(batch.candles.size());
    for (const Candle& c : batch.candles)
        sweep_setup_.bars.push_back(
            Bar{c.ts * 1'000'000'000, c.open, c.high, c.low, c.close, c.volume});
    sweep_setup_.waiting = false;
    sweep_setup_.ready = true;
}

void App::start_sweep_cell() {
    const int nx = static_cast<int>(sweep_.xs.size());
    const int ix = sweep_.done % nx;
    const int iy = sweep_.done / nx;
    BacktestConfig cfg = sweep_base_;   // copies bars (a few MB at worst)
    cfg.params[sweep_.px] = sweep_.xs[static_cast<size_t>(ix)];
    if (!sweep_.py.empty()) cfg.params[sweep_.py] = sweep_.ys[static_cast<size_t>(iy)];
    if (!engine_.start_backtest(std::move(cfg), sweep_strategy_)) {
        log_.add("sweep: engine busy, aborted");
        sweep_.running = false;
    }
}

// UI thread, before the panels draw (so a finished cell's result is consumed
// here and never stolen by the Backtest panel).
void App::pump_sweep() {
    // Fetched candles arrived: set up the grid and start cell 0.
    {
        std::lock_guard lock(pending_bt_mu_);
        if (sweep_setup_.ready) {
            sweep_setup_.ready = false;
            const SweepPanel::Request& rq = sweep_setup_.req;
            if (sweep_setup_.bars.size() < 3) {
                log_.add("sweep: not enough data for " + rq.symbol);
            } else {
                sweep_base_ = BacktestConfig{};
                sweep_base_.symbol = rq.symbol;
                sweep_base_.bars = std::move(sweep_setup_.bars);
                sweep_base_.initial_cash = rq.cash;
                sweep_base_.params = std::move(sweep_setup_.params);
                sweep_strategy_ = sweep_setup_.strategy;

                // Walk-forward split: the newest slice is held out of the
                // optimization entirely and scores the winner afterwards.
                sweep_test_bars_.clear();
                sweep_holdout_phase_ = false;
                double holdout = rq.holdout_pct;
                if (holdout > 0) {
                    const size_t n = sweep_base_.bars.size();
                    const size_t n_train =
                        static_cast<size_t>(n * (1.0 - holdout / 100.0));
                    if (n_train >= 3 && n - n_train >= 3) {
                        sweep_test_bars_.assign(sweep_base_.bars.begin() + n_train,
                                                sweep_base_.bars.end());
                        sweep_base_.bars.resize(n_train);
                    } else {
                        holdout = 0;
                        log_.add("sweep: too little data for a holdout, skipped");
                    }
                }

                sweep_ = SweepPanel::State{};
                sweep_.holdout_pct = holdout;
                sweep_.running = true;
                sweep_.px = rq.px;
                sweep_.py = rq.py;
                sweep_.metric = rq.metric;
                for (int i = 0; i < rq.nx; ++i)
                    sweep_.xs.push_back(rq.x0 + (rq.x1 - rq.x0) * i / (rq.nx - 1));
                if (!rq.py.empty())
                    for (int i = 0; i < rq.ny; ++i)
                        sweep_.ys.push_back(rq.y0 + (rq.y1 - rq.y0) * i / (rq.ny - 1));
                const int total = rq.nx * (rq.py.empty() ? 1 : rq.ny);
                sweep_.total = total;
                sweep_.vals.assign(static_cast<size_t>(total),
                                   std::numeric_limits<double>::quiet_NaN());
                sweep_.label = rq.symbol + " " + rq.interval + " " + rq.range + " — " +
                               strat_mgr_.display_name(sweep_setup_.key);
                start_sweep_cell();
            }
        }
    }

    if (!sweep_.running) return;
    // A mid-sweep rebuild is harmless now: the sweep's leased instance pins
    // its module until the last cell finishes.
    BacktestResult r;
    if (!engine_.take_result(r)) return;

    if (sweep_holdout_phase_) {   // the winner's run on unseen data
        sweep_.has_holdout = true;
        sweep_.holdout_val = sweep_metric_of(r, sweep_.metric);
        sweep_holdout_phase_ = false;
        sweep_.running = false;
        char buf[128];
        std::snprintf(buf, sizeof buf, "sweep: holdout %s %.4g (last %.0f%%, unseen)",
                      kSweepMetrics[sweep_.metric], sweep_.holdout_val,
                      sweep_.holdout_pct);
        log_.add(buf);
        return;
    }

    sweep_.vals[static_cast<size_t>(sweep_.done)] = sweep_metric_of(r, sweep_.metric);
    ++sweep_.done;
    if (sweep_.done < sweep_.total) {
        start_sweep_cell();
        return;
    }
    log_.add("sweep: finished " + std::to_string(sweep_.total) + " backtests (" +
             sweep_.label + ")");
    if (sweep_.holdout_pct <= 0 || sweep_test_bars_.empty()) {
        sweep_.running = false;
        return;
    }
    // Score the best cell on the held-out tail it never saw.
    int best = -1;
    for (int i = 0; i < sweep_.total; ++i) {
        if (best < 0) { best = i; continue; }
        const double a = sweep_.vals[static_cast<size_t>(i)];
        const double b = sweep_.vals[static_cast<size_t>(best)];
        if (sweep_metric_minimize(sweep_.metric) ? a < b : a > b) best = i;
    }
    const int nx = static_cast<int>(sweep_.xs.size());
    BacktestConfig cfg = sweep_base_;
    cfg.bars = sweep_test_bars_;
    cfg.params[sweep_.px] = sweep_.xs[static_cast<size_t>(best % nx)];
    if (!sweep_.py.empty())
        cfg.params[sweep_.py] = sweep_.ys[static_cast<size_t>(best / nx)];
    if (engine_.start_backtest(std::move(cfg), sweep_strategy_)) {
        sweep_holdout_phase_ = true;
    } else {
        log_.add("sweep: holdout run could not start (engine busy)");
        sweep_.running = false;
    }
}

App::~App() {
#ifdef _WIN32
    // Tear down the gateway + auto-login daemon we started (tied to app life).
    {
        const std::string args = ps_args("Stop-IbkrSession.ps1", true);
        if (!args.empty()) run_hidden(args, /*wait=*/true);
    }
#endif
    gw_.stop();
    if (signin_.worker.joinable()) signin_.worker.join();
    cfg_.watchlist = watchlist_.symbols();
    cfg_.chart_symbol = chart_.symbol();
    cfg_.chart_interval_idx = chart_.interval_idx();
    cfg_.chart_range_idx = chart_.range_idx();
    cfg_.backtest_cash = backtest_.cash();
    cfg_.trade_cash = trade_.cash();
    cfg_.trade_bar_sec = trade_.bar_sec();
    cfg_.trade_data_idx = trade_.data_idx();
    cfg_.trade_record = trade_.record();
    cfg_.risk_max_order_qty = trade_.risk().max_order_qty;
    cfg_.risk_max_position_qty = trade_.risk().max_position_qty;
    cfg_.risk_daily_max_loss = trade_.risk().daily_max_loss;
    cfg_.risk_max_drawdown_pct = trade_.risk_dd_pct();
    cfg_.risk_stale_feed_sec = trade_.risk().stale_feed_sec;
    cfg_.trade_symbols = trade_.symbols_config();
    cfg_.backtest_strategy = backtest_.strategy();
    cfg_.strategy_loaded = strat_mgr_.loaded_keys();
    cfg_.strategy_params = strat_mgr_.all_param_values();
    cfg_.save(config_path_);
}

void App::draw() {
    const ImGuiID dockspace_id =
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    // Surface engine/strategy/broker/feed log lines in the console; scan
    // them for alert-worthy events on the way through.
    std::string line;
    while (engine_.pop_log(line)) {
        alert_scan(line);
        log_.add(std::move(line));
    }
    if (ibkr_)
        while (ibkr_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (polygon_feed_)
        while (polygon_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (finnhub_feed_)
        while (finnhub_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (ibkr_feed_)
        while (ibkr_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    // Session over: stop streaming (frees the vendor connection slot).
    if ((polygon_feed_ || finnhub_feed_ || ibkr_feed_) && !engine_.live_running()) {
        rt_feed_active_.store(false, std::memory_order_relaxed);
        polygon_feed_.reset();
        finnhub_feed_.reset();
        ibkr_feed_.reset();
        log_.add("live: real-time feed stopped");
    }

    // TT_AUTORUN_BACKTEST=1: fire one AAPL backtest as soon as the feed is up
    // (headless end-to-end verification of the UI -> data -> engine path).
    if (!autorun_bt_done_ && gw_.connected() && std::getenv("TT_AUTORUN_BACKTEST")) {
        autorun_bt_done_ = true;
        queue_backtest("", "AAPL", "1d", "2y", 100'000.0);   // built-in SMA
        log_.add("autorun: queued AAPL 1d 2y backtest (built-in SMA)");
    }

    // TT_AUTORUN_SWEEP=1: 4x4 fast/slow grid — headless verification of the
    // sweep runner ("sweep: finished 16 backtests" on success).
    if (!autorun_sweep_done_ && gw_.connected() && std::getenv("TT_AUTORUN_SWEEP")) {
        autorun_sweep_done_ = true;
        SweepPanel::Request rq;
        rq.symbol = "AAPL";
        rq.interval = "1d";
        rq.range = "1y";
        rq.px = "fast";
        rq.x0 = 5;  rq.x1 = 20;  rq.nx = 4;
        rq.py = "slow";
        rq.y0 = 30; rq.y1 = 120; rq.ny = 4;
        rq.holdout_pct = 25;   // exercises the walk-forward phase headlessly
        queue_sweep(rq);
        log_.add("autorun: queued 4x4 fast/slow sweep");
    }

    // Journal: session rows are keyed off the live_running transition (works
    // for every start path — Trade panel, autorun, future ones), and fills
    // are drained afterwards so the session row always exists first.
    const bool live_now = engine_.live_running();
    if (!prev_live_running_ && live_now) {
        journal_syms_ = engine_.live_symbols();
        std::string jsyms;
        for (const std::string& sym : journal_syms_)
            jsyms += (jsyms.empty() ? "" : ",") + sym;
        journal_session_ = journal_.begin_session(
            jsyms, ibkr_ ? "ibkr" : "sim", engine_.live_snapshot().cash);
    } else if (prev_live_running_ && !live_now && journal_session_) {
        const LiveSnapshot s = engine_.live_snapshot();
        journal_.end_session(journal_session_, s.equity, s.halted);
        journal_session_ = 0;
    }
    prev_live_running_ = live_now;
    Engine::FillRecord fr;
    while (engine_.pop_fill(fr)) {
        const std::string sym = fr.symbol_id >= 1 && fr.symbol_id <= journal_syms_.size()
                                    ? journal_syms_[fr.symbol_id - 1]
                                    : "?";
        journal_.add_fill(journal_session_, fr.ts_ns, sym, fr.side == 1, fr.qty,
                          fr.price, fr.fee, fr.order_id);
    }

    pump_sweep();   // before the panels: sweep results must not be stolen

    draw_menu_bar();
    draw_account_modal();
    draw_data_modal();
    draw_trading_guards();
    if (show_chart_) {
        // Overlay fills for the charted symbol: last backtest + live session.
        std::vector<FillMarker> fills;
        const std::string chart_sym = chart_.symbol();
        if (const BacktestResult* r = backtest_.result(); r && r->symbol == chart_sym)
            for (const TradeRow& t : r->fills)
                fills.push_back({static_cast<double>(t.ts_ns) / 1e9, t.price,
                                 t.side == static_cast<uint8_t>(Side::Buy)});
        if (engine_.live_running()) {
            const LiveSnapshot s = engine_.live_snapshot();
            for (const OrderRecord& o : s.orders)
                if (o.status == OrderStatus::Filled && o.symbol == chart_sym)
                    fills.push_back({static_cast<double>(o.ts_ns) / 1e9, o.fill_price,
                                     o.side == static_cast<uint8_t>(Side::Buy)});
        }
        chart_.draw(&show_chart_, fills);
    }
    if (show_watchlist_)
        watchlist_.draw(&show_watchlist_, [this](const std::string& sym) {
            chart_.show_symbol(sym);
            backtest_.set_symbol(sym);
        });
    // Deferred strategy loads, strategy-switch backtests, and finished-run
    // instance cleanup advance every frame, independent of open panels.
    strat_mgr_.pump();
    pump_pending_run();
    pump_leases();

    if (show_backtest_)
        backtest_.draw(&show_backtest_, strat_mgr_.sources(),
                       [this](const std::string& k) { return strat_mgr_.loaded_fresh(k); },
                       pending_run_.active || strat_mgr_.load_pending(),
                       sweep_.running,
                       [this](const std::string& src, const std::string& sym,
                              const std::string& ivl, const std::string& rng,
                              double cash) {
                           queue_backtest_as(src, sym, ivl, rng, cash);
                       });
    if (show_replay_)
        replay_.draw(&show_replay_, strat_mgr_.loaded_keys(),
                     [this](const std::string& k) { return strat_mgr_.display_name(k); },
                     [this](const std::string& path, int bar_seconds_override,
                            const std::string& strat_key, double cash) {
                         TickLog log;
                         std::string err;
                         if (!tick_log_read(path, log, err)) {
                             log_.add("replay: " + err);
                             return;
                         }
                         ReplayConfig cfg;
                         cfg.name = "replay:" +
                                    std::filesystem::path(path).filename().string();
                         cfg.log = std::move(log);
                         cfg.bar_seconds_override = bar_seconds_override;
                         cfg.initial_cash = cash;
                         cfg.params = strat_mgr_.param_values(strat_key);
                         IStrategy* strat = acquire_strategy(strat_key);
                         if (!strat) {
                             log_.add("replay: strategy '" + strat_key + "' is not loaded");
                             return;
                         }
                         if (engine_.start_replay(std::move(cfg), strat)) {
                             // Replay shares the backtest engine slot/flag.
                             leases_.push_back({strat, strat_key, StrategyLease::Backtest});
                             log_.add("replay: running " + path + " (" +
                                      strat_mgr_.display_name(strat_key) + ")");
                         } else {
                             release_strategy({strat, strat_key, StrategyLease::Backtest});
                             log_.add("replay: engine busy, try again");
                         }
                     });
    if (show_sweep_)
        sweep_panel_.draw(&show_sweep_, strat_mgr_.loaded_keys(),
                          [this](const std::string& k) { return strat_mgr_.display_name(k); },
                          [this](const std::string& k) { return strat_mgr_.param_values(k); },
                          sweep_,
                          [this](const SweepPanel::Request& rq) { queue_sweep(rq); },
                          [this] {
                              sweep_.running = false;
                              log_.add("sweep: cancelled");
                          });
    if (show_strategy_) strat_mgr_.draw(&show_strategy_);
    if (show_build_output_) strat_mgr_.draw_build_output(&show_build_output_);
    if (show_trade_)
        trade_.draw(&show_trade_, strat_mgr_.loaded_keys(),
                    [this](const std::string& k) {
                        std::vector<TradePanel::StratParam> out;
                        for (const auto& s : strat_mgr_.param_specs(k))
                            out.push_back({s.name, s.value, s.min, s.max});
                        return out;
                    },
                    [this](const std::string& k) { return strat_mgr_.display_name(k); },
                    !polygon_key().empty(), !finnhub_key().empty(), gw_.connected(),
                    [&] {
                        TradePanel::AccountInfo a;
                        const auto ib = read_ibkr_accounts();
                        for (const auto& x : ib.accounts)
                            if (x.name == ib.active) { a.label = x.label; break; }
                        a.kind = static_cast<int>(gw_.account_kind());
                        a.readonly = ib.active_readonly();
                        a.subaccounts = gw_.accounts();
                        return a;
                    }(),
                    [this](const TradePanel::StartOpts& opts) {
                        std::vector<std::string> syms;
                        std::vector<int> sym_bars;
                        std::vector<std::string> sym_accts;
                        std::vector<RiskLimits> sym_risk;
                        bool any_record = false;
                        // Session-level equity/stale halts run on one portfolio, so
                        // drive them from the tightest (min non-zero) per-symbol value.
                        RiskLimits session_risk{};
                        session_risk.daily_max_loss = 0;
                        session_risk.max_drawdown_pct = 0;
                        session_risk.stale_feed_sec = 0;
                        auto tight = [](double cur, double v) {
                            return v > 0 && (cur == 0 || v < cur) ? v : cur;
                        };
                        for (const auto& so : opts.symbols) {
                            syms.push_back(so.symbol);
                            sym_bars.push_back(so.bar_seconds);
                            sym_accts.push_back(so.account);
                            sym_risk.push_back(so.risk);
                            any_record = any_record || so.record;
                            session_risk.daily_max_loss =
                                tight(session_risk.daily_max_loss, so.risk.daily_max_loss);
                            session_risk.max_drawdown_pct = tight(
                                session_risk.max_drawdown_pct, so.risk.max_drawdown_pct);
                            if (so.risk.stale_feed_sec > 0 &&
                                (session_risk.stale_feed_sec == 0 ||
                                 so.risk.stale_feed_sec < session_risk.stale_feed_sec))
                                session_risk.stale_feed_sec = so.risk.stale_feed_sec;
                        }
                        // Session default bar size (feed gap-backfill granularity);
                        // each symbol still aggregates at its own size below.
                        const int session_bar =
                            opts.symbols.empty() ? 60 : opts.symbols.front().bar_seconds;
                        LiveConfig cfg;
                        cfg.symbols = syms;
                        cfg.initial_cash = opts.session_cash;
                        // Session-level params stay empty: every symbol carries its
                        // own map in symbol_params (ctx.param resolves per symbol).
                        cfg.bar_seconds = session_bar;
                        cfg.symbol_bar_seconds = sym_bars;
                        cfg.risk = session_risk;
                        cfg.symbol_risk = sym_risk;
                        // Every data source is real-time now => spin the engine
                        // thread; ticks are handled in ns, not after Sleep(5).
                        cfg.busy_spin = true;
                        // Optional core pinning (TT_PIN_ENGINE / TT_PIN_FEED =
                        // core index): kills scheduler-migration jitter.
                        if (const char* pin = std::getenv("TT_PIN_ENGINE"))
                            cfg.pin_core = std::atoi(pin);
                        // Per-strategy callback watchdog (huge headroom over the
                        // µs a normal callback takes; catches runaways only).
                        cfg.watchdog_ms = 250;
                        if (const char* w = std::getenv("TT_WATCHDOG_MS"))
                            cfg.watchdog_ms = std::atoi(w);
                        if (any_record) {
                            std::error_code ec;
                            std::filesystem::create_directories(sessions_dir(), ec);
                            char name[32];
                            const std::time_t now = std::time(nullptr);
                            std::tm tm{};
                            localtime_s(&tm, &now);
                            std::strftime(name, sizeof name, "%Y%m%d_%H%M%S.ttk", &tm);
                            cfg.capture_path = sessions_dir() + "\\" + name;
                        }
                        std::unique_ptr<IbkrBroker> ibkr_broker;
                        if (opts.broker == TradePanel::Broker::Ibkr) {
                            IbkrConfig ic;
                            if (const char* gw = std::getenv("TT_IBKR_GATEWAY"))
                                ic.gateway_url = gw;
                            ic.symbols = syms;
                            ic.symbol_accounts = sym_accts;   // per-symbol sub-account routing
                            ic.read_only = read_ibkr_accounts().active_readonly();
                            ibkr_broker = std::make_unique<IbkrBroker>(std::move(ic));
                            cfg.broker = ibkr_broker.get();
                            log_.add(ic.read_only
                                         ? "live: IBKR account is READ-ONLY — orders blocked"
                                         : "live: routing orders to IBKR gateway");
                        }
                        // Each symbol needs its strategy built + loaded first.
                        std::string unbuilt;
                        for (const auto& so : opts.symbols)
                            if (!so.strat_key.empty() &&
                                !strat_mgr_.loaded_fresh(so.strat_key)) {
                                strat_mgr_.request_load(so.strat_key);
                                unbuilt += (unbuilt.empty() ? "" : ", ") + so.strat_key;
                            }
                        if (!unbuilt.empty()) {
                            log_.add("live: building strategies (" + unbuilt +
                                     ") — click Start Trading again");
                            return;
                        }
                        // One strategy instance + param set per symbol.
                        std::vector<IStrategy*> strategies;
                        std::vector<StrategyLease> new_leases;
                        bool acq_ok = true;
                        for (const auto& so : opts.symbols) {
                            IStrategy* inst = acquire_strategy(so.strat_key);
                            if (!inst) {
                                log_.add("live: strategy '" +
                                         strat_mgr_.display_name(so.strat_key) +
                                         "' failed to load");
                                acq_ok = false;
                                break;
                            }
                            strategies.push_back(inst);
                            cfg.symbol_params.push_back(
                                so.params.empty() ? strat_mgr_.param_values(so.strat_key)
                                                   : so.params);
                            new_leases.push_back({inst, so.strat_key, StrategyLease::Live});
                        }
                        if (!acq_ok) {
                            for (const auto& l : new_leases) release_strategy(l);
                            return;
                        }
                        if (engine_.start_live(std::move(cfg), std::move(strategies))) {
                            for (const auto& l : new_leases) leases_.push_back(l);
                            // Previous session's thread was joined inside
                            // start_live, so replacing the old broker is safe.
                            ibkr_ = std::move(ibkr_broker);
                            polygon_feed_.reset();   // previous session's feeds
                            finnhub_feed_.reset();
                            ibkr_feed_.reset();
                            rt_feed_active_.store(false, std::memory_order_relaxed);
                            const auto sink = [this](const EngineEvent& ev) {
                                return engine_.push_feed_event(ev);
                            };
                            if (opts.data == TradePanel::DataFeed::Polygon &&
                                !polygon_key().empty()) {
                                PolygonFeedConfig pc;
                                pc.api_key = polygon_key();
                                // e.g. wss://delayed.polygon.io/stocks to test the
                                // adapter on the $29 delayed tier (same protocol).
                                if (const char* ws = std::getenv("TT_POLYGON_WS"))
                                    pc.stream_url = ws;
                                pc.symbols = syms;
                                pc.busy_poll = std::getenv("TT_FEED_SPIN") != nullptr;
                                if (const char* pin = std::getenv("TT_PIN_FEED"))
                                    pc.pin_core = std::atoi(pin);
                                pc.bar_seconds = session_bar;
                                polygon_feed_ =
                                    std::make_unique<PolygonFeed>(std::move(pc), sink);
                                polygon_feed_->start();
                                rt_feed_active_.store(true, std::memory_order_relaxed);
                            } else if (opts.data == TradePanel::DataFeed::Finnhub &&
                                       !finnhub_key().empty()) {
                                FinnhubFeedConfig fc;
                                fc.api_key = finnhub_key();
                                if (const char* ws = std::getenv("TT_FINNHUB_WS"))
                                    fc.stream_url = ws;
                                fc.symbols = syms;
                                fc.busy_poll = std::getenv("TT_FEED_SPIN") != nullptr;
                                if (const char* pin = std::getenv("TT_PIN_FEED"))
                                    fc.pin_core = std::atoi(pin);
                                fc.bar_seconds = session_bar;
                                finnhub_feed_ =
                                    std::make_unique<FinnhubFeed>(std::move(fc), sink);
                                finnhub_feed_->start();
                                rt_feed_active_.store(true, std::memory_order_relaxed);
                            } else if (opts.data == TradePanel::DataFeed::Ibkr) {
                                IbkrFeedConfig fc;
                                if (const char* gw = std::getenv("TT_IBKR_GATEWAY")) {
                                    fc.gateway_url = gw;
                                    // wss://host/v1/api/ws mirrors the REST base.
                                    std::string ws = fc.gateway_url;
                                    if (ws.rfind("https://", 0) == 0)
                                        ws.replace(0, 8, "wss://");
                                    fc.ws_url = ws + "/ws";
                                }
                                fc.symbols = syms;
                                fc.bar_seconds = session_bar;
                                if (const char* pin = std::getenv("TT_PIN_FEED"))
                                    fc.pin_core = std::atoi(pin);
                                ibkr_feed_ =
                                    std::make_unique<IbkrFeed>(std::move(fc), sink);
                                ibkr_feed_->start();
                                rt_feed_active_.store(true, std::memory_order_relaxed);
                            }
                            for (const std::string& sym : syms)
                                watchlist_.ensure(sym);  // quote subscription feeds the engine
                            // Log each symbol with the strategy it runs.
                            std::string joined;
                            for (const auto& so : opts.symbols)
                                joined += (joined.empty() ? "" : ", ") + so.symbol + ":" +
                                          strat_mgr_.display_name(so.strat_key);
                            log_.add("live: session queued for " + joined);
                        } else {
                            for (const auto& l : new_leases) release_strategy(l);
                            log_.add("live: cannot start (engine busy)");
                        }
                    });
    if (show_blotter_) blotter_.draw(&show_blotter_);
    if (show_positions_) positions_.draw(&show_positions_);
    if (show_journal_) journal_panel_.draw(&show_journal_);
    if (show_log_) log_.draw("Log Console", &show_log_);

#ifdef TT_DEBUG
    // Debug menu (or TT_SIM_TICKS=1): synthesize a 2 Hz random walk for the
    // live session — demo/verification when the market is closed.
    if (engine_.live_running() && sim_ticks_) {
        const double now = ImGui::GetTime();
        if (now >= sim_tick_next_s_) {
            sim_tick_next_s_ = now + 0.5;
            if (sim_tick_px_ <= 0.0) sim_tick_px_ = 100.0;
            sim_tick_rng_ = sim_tick_rng_ * 1664525u + 1013904223u;
            sim_tick_px_ += (static_cast<double>(sim_tick_rng_ >> 8 & 0xffff) / 65535.0 - 0.5);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            for (const std::string& sym : engine_.live_symbols())
                engine_.push_live_tick(sym, ms, sim_tick_px_, 0.0);
        }
    }
#endif

    // TT_AUTORUN_LIVE=1: start a session, manual buy, kill switch — headless
    // verification of the whole live path.
    if (std::getenv("TT_AUTORUN_LIVE")) {
        const LiveSnapshot s = engine_.live_snapshot();
        if (autorun_live_stage_ == 0) {
            autorun_live_stage_ = 1;
            LiveConfig cfg;
            cfg.symbols = {"SIMTEST", "SIMTEST2"};   // proves multi-symbol routing headlessly
            cfg.params = strat_mgr_.param_values("");   // built-in SMA
            cfg.bar_seconds = 2;
            if (IStrategy* strat = acquire_strategy("")) {
                // Both SIMTEST symbols run the same one strategy instance here.
                if (engine_.start_live(std::move(cfg),
                                       std::vector<IStrategy*>{strat, strat}))
                    leases_.push_back({strat, "", StrategyLease::Live});
                else
                    release_strategy({strat, "", StrategyLease::Live});
            }
            log_.add("autorun-live: session started");
        } else if (autorun_live_stage_ == 1 && s.ticks >= 3) {
            autorun_live_stage_ = 2;
            engine_.submit_manual(1, true, 10);
            log_.add("autorun-live: manual BUY 10 SIMTEST submitted");
        } else if (autorun_live_stage_ == 2 && !s.symbols.empty() &&
                  s.symbols[0].position.qty > 0) {
            autorun_live_stage_ = 3;
            log_.add("autorun-live: position open, firing kill switch");
            engine_.kill_switch();
        } else if (autorun_live_stage_ == 3 && s.halted) {
            bool all_flat = true;
            for (const SymbolState& sym : s.symbols)
                if (sym.position.qty != 0) all_flat = false;
            if (all_flat) {
                autorun_live_stage_ = 4;
                log_.add("autorun-live: FLAT after kill switch — live path verified");
            }
        }
    }
    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::request_quit() {
    if (engine_.live_running()) pending_quit_ = true;   // draw_trading_guards() confirms
    else should_quit_ = true;
}

void App::safe_stop_live() {
    if (!engine_.live_running()) return;
    engine_.kill_switch();   // cancel all + flatten + halt strategy
    engine_.stop_live();     // graceful stop, joins the live thread
    log_.add("account: stopped live trading (kill switch)");
}

void App::do_ibkr_signout() {
    const std::string args = signout_args();
    if (!args.empty()) {
        run_hidden(args);
        log_.add("account: signing out of IBKR (stopping auto-login, gateway logout)");
    } else {
        log_.add("account: sign-out script missing (scripts\\Stop-IbkrLogin.ps1)");
    }
}

// Top-level confirm dialogs so signing out or quitting while a live trading
// session is running flattens + stops it first (same guard as switching).
void App::draw_trading_guards() {
    if (pending_signout_ && !ImGui::IsPopupOpen("Confirm sign out"))
        ImGui::OpenPopup("Confirm sign out");
    if (ImGui::BeginPopupModal("Confirm sign out", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Signing out will cancel "
                           "open orders, flatten positions, and stop it.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & sign out")) {
            safe_stop_live();
            do_ibkr_signout();
            pending_signout_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_signout_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (pending_quit_ && !ImGui::IsPopupOpen("Confirm quit"))
        ImGui::OpenPopup("Confirm quit");
    if (ImGui::BeginPopupModal("Confirm quit", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Quitting will cancel open "
                           "orders, flatten positions, and stop it before exit.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & quit")) {
            safe_stop_live();
            pending_quit_ = false;
            should_quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep running")) {
            pending_quit_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::draw_account_menu() {
    if (!ImGui::BeginMenu("Account")) return;
    if (gw_.connected()) {
        const std::string acct = gw_.account();
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "ibkr: session active%s",
                           acct.empty() ? "" : ("  (" + acct + ")").c_str());
        badge_kind(gw_.account_kind());   // PAPER (green) / LIVE (red)
        if (read_ibkr_accounts().active_readonly()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "READ-ONLY");
        }
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "ibkr: no gateway session");
    }
    ImGui::Separator();

    // Account switching lives in the dialog (pick an account, click it).
    if (ImGui::MenuItem(gw_.connected() ? "Switch" : "Sign In"))
        signin_.account_request_open = true;

    const auto ibkr = read_ibkr_accounts();

    if (ImGui::BeginMenu("Sign Out", gw_.connected())) {
        const std::string acct = gw_.account();
        const std::string label =
            "IBKR gateway" + (acct.empty() ? std::string() : ": " + acct);
        if (ImGui::MenuItem(label.c_str())) {
            if (engine_.live_running()) pending_signout_ = true;  // confirm + safe-stop
            else do_ibkr_signout();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Remove", !ibkr.accounts.empty())) {
        ImGui::TextDisabled("Deletes the saved credentials");
        for (const auto& acc : ibkr.accounts) {
            const std::string label = "IBKR: " + acc.label + "##" + acc.name;
            if (ImGui::MenuItem(label.c_str())) {
                const std::string args = ps_args("Remove-IbkrAccount.ps1", true,
                                                 "-Account \"" + acc.name + "\"");
                if (!args.empty()) {
                    run_hidden(args);
                    log_.add("account: removing IBKR '" + acc.name + "'");
                }
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Data feeds (Polygon / Finnhub API keys) — separate from the broker Account
// menu. IBKR gateway data needs no key and is picked in the Trade panel.
void App::draw_data_menu() {
    if (!ImGui::BeginMenu("Data")) return;
    const std::string poly = accounts_.active_name("polygon");
    const std::string finn = accounts_.active_name("finnhub");
    ImGui::TextDisabled("polygon: %s", poly.empty() ? "(not signed in)" : poly.c_str());
    ImGui::TextDisabled("finnhub: %s", finn.empty() ? "(not signed in)" : finn.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem("Add Feed"))
        signin_.data_request_open = true;

    if (ImGui::BeginMenu("Sign Out", !poly.empty() || !finn.empty())) {
        if (!poly.empty() && ImGui::MenuItem(("polygon: " + poly).c_str())) {
            accounts_.sign_out("polygon");
            log_.add("account: signed out of polygon");
        }
        if (!finn.empty() && ImGui::MenuItem(("finnhub: " + finn).c_str())) {
            accounts_.sign_out("finnhub");
            log_.add("account: signed out of finnhub");
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Remove", !accounts_.list().empty())) {
        ImGui::TextDisabled("Deletes the saved credentials");
        for (const auto& e : accounts_.list()) {
            const std::string label = e.name + "  (" + e.provider + ")";
            if (ImGui::MenuItem(label.c_str())) {
                accounts_.remove(e.name);
                log_.add("account: removed '" + e.name + "'");
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Route noteworthy engine/broker/feed log lines to the alert channel.
// Critical = money is at risk right now; Warning = something needs a look;
// Info = fills (webhook only, no beep — they can be frequent).
void App::alert_scan(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    if (has("KILL SWITCH") || has("RISK HALT") || has("WATCHDOG"))
        alerts_.notify(AlertNotifier::Critical, l);
    else if (has("rejected") || has("stream lost") || has("auth failed") ||
             has("(drops!)"))
        alerts_.notify(AlertNotifier::Warning, l);
    else if (has("live: fill"))
        alerts_.notify(AlertNotifier::Info, l);
}

void App::refresh_ibkr_accounts() {
    const auto a = read_ibkr_accounts();
    signin_.ibkr_accounts.clear();
    signin_.ibkr_labels.clear();
    signin_.ibkr_paper.clear();
    signin_.ibkr_readonly.clear();
    for (const auto& acc : a.accounts) {
        signin_.ibkr_accounts.push_back(acc.name);
        signin_.ibkr_labels.push_back(acc.label);
        signin_.ibkr_paper.push_back(acc.paper ? 1 : 0);
        signin_.ibkr_readonly.push_back(acc.readonly ? 1 : 0);
    }
    signin_.ibkr_active = a.active;
    const int n = static_cast<int>(signin_.ibkr_accounts.size());
    if (signin_.ibkr_selected >= n) signin_.ibkr_selected = -1;
    if (signin_.ibkr_selected < 0) {   // default to the active account
        for (int i = 0; i < n; ++i)
            if (signin_.ibkr_accounts[i] == a.active) { signin_.ibkr_selected = i; break; }
        if (signin_.ibkr_selected < 0 && n > 0) signin_.ibkr_selected = 0;
    }
}

void App::draw_account_modal() {
    if (signin_.account_request_open) {
        signin_.account_request_open = false;
        signin_.account_open = true;
        ImGui::OpenPopup("Account");
    }
    // p_open shows a title-bar X (closes the popup like a normal window).
    if (!ImGui::BeginPopupModal("Account", &signin_.account_open,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Don't fade the app behind this dialog (see draw_data_modal for the why).
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = 0.0f;

    // Provider dropdown: only IBKR today, kept so more brokers can slot in.
    static constexpr const char* kBrokers[] = {"Interactive Brokers Canada"};
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Provider", &signin_.broker_provider, kBrokers, IM_ARRAYSIZE(kBrokers));
    ImGui::Separator();

    refresh_ibkr_accounts();   // pick up accounts added via "Add New"

    const bool up = gw_.connected();
    const bool initializing = !up && ImGui::GetTime() < gateway_starting_until_;

    // Kick off a sign-in (disconnected) or switch (connected, different
    // account) for `name`. Both run Switch-IbkrAccount, which tears the old
    // session down first.
    auto do_switch = [&](const std::string& name) {
        const std::string args = switch_args(name);
        if (args.empty()) return;
        run_hidden(args);
        gateway_starting_until_ = ImGui::GetTime() + 90.0;
        log_.add(std::string("account: ") +
                 (up ? "switching to IBKR '" : "signing in to IBKR '") + name + "'");
    };

    if (signin_.ibkr_accounts.empty()) {
        ImGui::TextDisabled("None saved yet - click \"Add New\".");
    } else {
        const float ch = ImGui::GetFontSize();
        for (int i = 0; i < static_cast<int>(signin_.ibkr_accounts.size()); ++i) {
            const std::string& name = signin_.ibkr_accounts[i];
            // Display the friendly label but ID the row by the unique key,
            // so two accounts sharing a label (paper + live) don't collide.
            const std::string row = signin_.ibkr_labels[i] + "##" + name;
            const bool active = up && name == signin_.ibkr_active;
            // A real checkmark (drawn with primitives, no font glyph) marks
            // the currently signed-in account, on the left of the row.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (active)
                ImGui::RenderCheckMark(ImGui::GetWindowDrawList(),
                                       ImVec2(p.x + 2.0f, p.y + ImGui::GetStyle().FramePadding.y),
                                       ImGui::GetColorU32(ImGuiCol_CheckMark), ch * 0.72f);
            ImGui::Dummy(ImVec2(ch + 4.0f, ch));
            ImGui::SameLine();
            // Clicking a row signs in to it (or switches to it); clicking the
            // already-signed-in one does nothing. Confirm first if a live
            // trading session is running.
            if (ImGui::Selectable(row.c_str(), active,
                                  ImGuiSelectableFlags_DontClosePopups, ImVec2(190, 0)) &&
                !active && !initializing) {
                if (engine_.live_running()) {
                    signin_.pending_account = name;
                    ImGui::OpenPopup("Confirm account change");
                } else {
                    do_switch(name);
                }
            }
            // PAPER/LIVE badge, then the read-only tag pinned to a fixed
            // column so it lines up whether the badge is "PAPER" or "LIVE".
            ImGui::SameLine();
            const float badge_x = ImGui::GetCursorPosX();
            const bool paper = signin_.ibkr_paper[i] != 0;
            ImGui::TextColored(paper ? ImVec4(0.25f, 0.85f, 0.45f, 1)
                                     : ImVec4(0.95f, 0.30f, 0.25f, 1),
                               paper ? "PAPER" : "LIVE");
            if (signin_.ibkr_readonly[i]) {
                ImGui::SameLine(badge_x + ImGui::CalcTextSize("PAPER").x +
                                ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "read-only");
            }
        }
    }

    // Live trading is active: flatten + stop it, then switch.
    if (ImGui::BeginPopupModal("Confirm account change", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Continuing will cancel "
                           "open orders, flatten positions, and stop the session before "
                           "changing accounts.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & switch")) {
            safe_stop_live();
            do_switch(signin_.pending_account);
            signin_.pending_account.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            signin_.pending_account.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::Button("Add New")) {
        const std::string args = addnew_args();
        if (!args.empty()) {
            ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(),
                          nullptr, SW_SHOWNORMAL);   // visible: prompts for credentials
            log_.add("account: add IBKR account (enter credentials in the console)");
        } else {
            log_.add("account: Save-IbkrCred.ps1 not found");
        }
    }
    ImGui::EndPopup();
}

void App::draw_data_modal() {
    if (signin_.data_request_open) {
        signin_.data_request_open = false;
        signin_.status.store(0);
        {
            std::lock_guard lock(signin_.mu);
            signin_.detail.clear();
        }
        signin_.data_open = true;
        ImGui::OpenPopup("Data");
    }
    if (!ImGui::BeginPopupModal("Data", &signin_.data_open,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Don't fade the app behind this dialog. Reassert here — only while it's
    // open, so there's no per-frame cost otherwise — so a runtime style change
    // (e.g. the Style Editor) can't bring the fade back.
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = 0.0f;

    auto make_account = [&] {
        Account a;
        a.name = signin_.name[0] ? signin_.name : "default";
        // Provider dropdown: 0 = Polygon, 1 = Finnhub.
        a.provider = signin_.provider == 1 ? "finnhub" : "polygon";
        a.key_id = signin_.key;
        a.secret = "";
        return a;
    };

    const int status = signin_.status.load();
    if (status == 2) {   // worker verified the credentials — save and close
        if (signin_.worker.joinable()) signin_.worker.join();
        const Account a = make_account();
        accounts_.upsert(a, /*make_active=*/true);
        std::string detail;
        {
            std::lock_guard lock(signin_.mu);
            detail = signin_.detail;
        }
        log_.add("account: signed in to " + a.provider + " as '" + a.name + "' (" +
                 detail + ")");
        signin_.status.store(0);
        signin_.secret[0] = '\0';   // no plaintext left in the UI buffer
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool busy = status == 1;
    ImGui::BeginDisabled(busy);
    static constexpr const char* kFeeds[] = {"Polygon", "Finnhub"};
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Provider", &signin_.provider, kFeeds, IM_ARRAYSIZE(kFeeds));
    ImGui::SetItemTooltip("Real-time market-data source. The IBKR gateway is also a "
                          "feed, needs no key, and is picked in the Trade panel.");
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("Name", signin_.name, sizeof signin_.name);
    ImGui::SetItemTooltip("Label shown in the Data menu (e.g. \"data\")");
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("API key", signin_.key, sizeof signin_.key,
                     ImGuiInputTextFlags_Password);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Stored encrypted (Windows DPAPI, this user only).");

    if (busy) {
        ImGui::TextDisabled("Verifying...");
    } else if (status == 3) {
        std::lock_guard lock(signin_.mu);
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "Failed: %s",
                           signin_.detail.c_str());
    }

    const bool have_input = signin_.key[0] != '\0';
    ImGui::BeginDisabled(busy || !have_input);
    if (ImGui::Button("Verify & Sign In")) {
        if (signin_.worker.joinable()) signin_.worker.join();
        signin_.status.store(1);
        const std::string key = signin_.key;
        const int prov = signin_.provider;
        signin_.worker = std::thread([this, key, prov] {
            std::string detail;
            const bool ok = prov == 1
                ? finnhub_verify_key(FinnhubFeedConfig{}.rest_url, key, detail)
                : polygon_verify_key(PolygonFeedConfig{}.rest_url, key, detail);
            {
                std::lock_guard lock(signin_.mu);
                signin_.detail = std::move(detail);
            }
            signin_.status.store(ok ? 2 : 3);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Save without verifying")) {
        const Account a = make_account();
        accounts_.upsert(a, /*make_active=*/true);
        log_.add("account: saved " + a.provider + " '" + a.name + "' (not verified)");
        signin_.secret[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);   // let the worker finish; it writes signin_ state
    if (ImGui::Button("Cancel")) {
        signin_.secret[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    draw_account_menu();
    draw_data_menu();
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Watchlist", nullptr, &show_watchlist_);
        ImGui::MenuItem("Backtest", nullptr, &show_backtest_);
        ImGui::MenuItem("Replay", nullptr, &show_replay_);
        ImGui::MenuItem("Optimizer", nullptr, &show_sweep_);
        ImGui::MenuItem("Strategy", nullptr, &show_strategy_);
        ImGui::MenuItem("Build Output", nullptr, &show_build_output_);
        ImGui::MenuItem("Trade", nullptr, &show_trade_);
        ImGui::MenuItem("Blotter", nullptr, &show_blotter_);
        ImGui::MenuItem("Positions", nullptr, &show_positions_);
        ImGui::MenuItem("Journal", nullptr, &show_journal_);
        ImGui::MenuItem("Log Console", nullptr, &show_log_);
        ImGui::Separator();
        bool alerts_on = !alerts_.muted();
        if (ImGui::MenuItem("Alerts", nullptr, &alerts_on)) alerts_.set_muted(!alerts_on);
        ImGui::SetItemTooltip(alerts_.has_webhook()
                                  ? "Beep + webhook on halts, rejects, disconnects, fills"
                                  : "Beeps on halts/rejects/disconnects. Set "
                                    "\"alert_webhook\" in config.json (or "
                                    "TT_ALERT_WEBHOOK) for phone push, e.g. an "
                                    "ntfy.sh topic URL");
        ImGui::EndMenu();
    }
#ifdef TT_DEBUG
    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("Simulate ticks", nullptr, &sim_ticks_);
        ImGui::SetItemTooltip("Feed the live session a 2 Hz random walk — "
                              "test strategies while the market is closed");
        ImGui::EndMenu();
    }
#endif
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImPlot Demo", nullptr, &show_implot_demo_);
        ImGui::EndMenu();
    }

    // Right-aligned gateway session indicator.
    const bool up = gw_.connected();
    const bool initializing = !up && ImGui::GetTime() < gateway_starting_until_;
    const char* label = up ? "GATEWAY UP" : (initializing ? "INITIALIZING" : "GATEWAY DOWN");
    const ImVec4 col = up ? ImVec4(0.25f, 0.85f, 0.45f, 1.0f)
                          : (initializing ? ImVec4(0.95f, 0.80f, 0.25f, 1.0f)
                                          : ImVec4(0.95f, 0.35f, 0.25f, 1.0f));
    const float w = ImGui::CalcTextSize(label).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextColored(col, "%s", label);
    ImGui::EndMainMenuBar();
}

void App::setup_default_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.34f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Watchlist", left);
    ImGui::DockBuilderDockWindow("Chart", center);
    ImGui::DockBuilderDockWindow("Backtest", right);
    ImGui::DockBuilderDockWindow("Strategy", right);
    ImGui::DockBuilderDockWindow("Trade", right);
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderDockWindow("Blotter", bottom);
    ImGui::DockBuilderDockWindow("Positions", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
