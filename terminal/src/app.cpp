#include "app.h"
#include "dev_paths.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout) + private dock node flags
#include "implot.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
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
} // namespace

std::optional<Account> App::alpaca_creds() const {
    if (auto a = accounts_.active("alpaca")) return a;
    const char* k = std::getenv("APCA_API_KEY_ID");
    const char* s = std::getenv("APCA_API_SECRET_KEY");
    if (k && *k && s && *s) return Account{"env", "alpaca", k, s};
    return std::nullopt;
}

std::string App::polygon_key() const {
    if (auto a = accounts_.active("polygon")) return a->key_id;
    const char* k = std::getenv("POLYGON_API_KEY");
    return k && *k ? k : "";
}

App::App(std::string python_cmd, std::string service_dir)
    : ipc_(std::move(python_cmd), std::move(service_dir)),
      host_(gxx_path(), std::string(TT_REPO_ROOT) + "/sdk/include", strategies_out_dir()),
      chart_(ipc_, series_),
      watchlist_(ipc_, quotes_),
      backtest_(engine_),
      strat_mgr_(host_, engine_, std::string(TT_REPO_ROOT) + "/strategies"),
      trade_(engine_, sessions_dir()),
      blotter_(engine_),
      positions_(engine_),
      sweep_panel_(engine_),
      accounts_((data_dir() / "accounts.json").string()) {
    net::IpcClient::Callbacks cbs;
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
    trade_.restore(cfg_.trade_cash, cfg_.trade_bar_sec);
    {
        RiskLimits r;
        r.max_order_qty = cfg_.risk_max_order_qty;
        r.max_position_qty = cfg_.risk_max_position_qty;
        r.daily_max_loss = cfg_.risk_daily_max_loss;
        r.stale_feed_sec = cfg_.risk_stale_feed_sec;
        trade_.restore_risk(r, cfg_.risk_max_drawdown_pct);
    }
    const char* wh = std::getenv("TT_ALERT_WEBHOOK");
    alerts_.set_webhook(wh && *wh ? wh : cfg_.alert_webhook);
    if (alerts_.has_webhook()) log_.add("alerts: webhook configured");

    if (!journal_.open((data_dir() / "journal.db").string()))
        log_.add("journal: could not open journal.db — history disabled");

#ifdef TT_DEBUG
    sim_ticks_ = std::getenv("TT_SIM_TICKS") != nullptr;
#endif

    ipc_.start(std::move(cbs));
}

// IPC thread: if this candle batch is the one a queued backtest is waiting
// for, convert it and launch the engine.
void App::start_pending_backtest(net::CandleBatch& batch) {
    PendingBacktest bt;
    {
        std::lock_guard lock(pending_bt_mu_);
        if (!pending_bt_.active || pending_bt_.symbol != batch.symbol ||
            pending_bt_.interval != batch.interval)
            return;
        bt = pending_bt_;
        pending_bt_.active = false;
    }
    if (batch.candles.size() < 3) {
        log_.add("backtest: not enough data for " + batch.symbol);
        return;
    }
    if (bt.strategy_gen != host_.generation() || !bt.strategy) {
        log_.add("backtest: strategy changed while fetching data, cancelled");
        return;
    }
    BacktestConfig cfg;
    cfg.symbol = batch.symbol;
    cfg.bars.reserve(batch.candles.size());
    for (const Candle& c : batch.candles)
        cfg.bars.push_back(Bar{c.ts * 1'000'000'000, c.open, c.high, c.low,
                               c.close, c.volume});
    cfg.initial_cash = bt.cash;
    cfg.params = std::move(bt.params);
    if (!engine_.start_backtest(std::move(cfg), bt.strategy))
        log_.add("backtest: engine busy, try again");
}

// UI thread: capture the active strategy + params and fetch the data.
void App::queue_backtest(const std::string& sym, const std::string& ivl,
                         const std::string& rng, double cash) {
    if (!ipc_.connected()) {
        log_.add("backtest: feed is down, cannot fetch data");
        return;
    }
    {
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_ = {true, sym, ivl, strat_mgr_.param_values(), cash,
                       strat_mgr_.active_strategy(sma_), host_.generation()};
    }
    ipc_.request_candles(sym, ivl, rng);
}

// ------------------------------------------------------------------ sweep

// UI thread: capture the request + strategy and fetch the data.
void App::queue_sweep(const SweepPanel::Request& rq) {
    if (!ipc_.connected()) {
        log_.add("sweep: feed is down, cannot fetch data");
        return;
    }
    if (sweep_.running || engine_.running()) {
        log_.add("sweep: engine busy, try again");
        return;
    }
    {
        std::lock_guard lock(pending_bt_mu_);
        sweep_setup_ = SweepSetup{};
        sweep_setup_.waiting = true;
        sweep_setup_.req = rq;
        sweep_setup_.strategy = strat_mgr_.active_strategy(sma_);
        sweep_setup_.strategy_gen = host_.generation();
    }
    ipc_.request_candles(rq.symbol, rq.interval, rq.range);
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
            } else if (sweep_setup_.strategy_gen != host_.generation()) {
                log_.add("sweep: strategy changed while fetching data, cancelled");
            } else {
                sweep_base_ = BacktestConfig{};
                sweep_base_.symbol = rq.symbol;
                sweep_base_.bars = std::move(sweep_setup_.bars);
                sweep_base_.initial_cash = rq.cash;
                sweep_base_.params = strat_mgr_.param_values();
                sweep_strategy_ = sweep_setup_.strategy;
                sweep_gen_ = sweep_setup_.strategy_gen;

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
                               strat_mgr_.active_name();
                start_sweep_cell();
            }
        }
    }

    if (!sweep_.running) return;
    if (sweep_gen_ != host_.generation()) {
        log_.add("sweep: strategy recompiled mid-sweep, aborted");
        sweep_.running = false;
        sweep_holdout_phase_ = false;
        return;
    }
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
    ipc_.stop();
    if (signin_.worker.joinable()) signin_.worker.join();
    cfg_.watchlist = watchlist_.symbols();
    cfg_.chart_symbol = chart_.symbol();
    cfg_.chart_interval_idx = chart_.interval_idx();
    cfg_.chart_range_idx = chart_.range_idx();
    cfg_.backtest_cash = backtest_.cash();
    cfg_.trade_cash = trade_.cash();
    cfg_.trade_bar_sec = trade_.bar_sec();
    cfg_.risk_max_order_qty = trade_.risk().max_order_qty;
    cfg_.risk_max_position_qty = trade_.risk().max_position_qty;
    cfg_.risk_daily_max_loss = trade_.risk().daily_max_loss;
    cfg_.risk_max_drawdown_pct = trade_.risk_dd_pct();
    cfg_.risk_stale_feed_sec = trade_.risk().stale_feed_sec;
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
    if (alpaca_)
        while (alpaca_->pop_log(line)) {
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
    if (ibkr_feed_)
        while (ibkr_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (alpaca_feed_)
        while (alpaca_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    // Session over: stop streaming (frees the vendor connection slot).
    if ((alpaca_feed_ || polygon_feed_ || ibkr_feed_) && !engine_.live_running()) {
        rt_feed_active_.store(false, std::memory_order_relaxed);
        alpaca_feed_.reset();
        polygon_feed_.reset();
        ibkr_feed_.reset();
        log_.add("live: real-time feed stopped");
    }

    // TT_AUTORUN_BACKTEST=1: fire one AAPL backtest as soon as the feed is up
    // (headless end-to-end verification of the UI -> data -> engine path).
    if (!autorun_bt_done_ && ipc_.connected() && std::getenv("TT_AUTORUN_BACKTEST")) {
        autorun_bt_done_ = true;
        queue_backtest("AAPL", "1d", "2y", 100'000.0);
        log_.add("autorun: queued AAPL 1d 2y backtest (" + strat_mgr_.active_name() + ")");
    }

    // TT_AUTORUN_SWEEP=1: 4x4 fast/slow grid — headless verification of the
    // sweep runner ("sweep: finished 16 backtests" on success).
    if (!autorun_sweep_done_ && ipc_.connected() && std::getenv("TT_AUTORUN_SWEEP")) {
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
            jsyms, alpaca_ ? "alpaca" : (ibkr_ ? "ibkr" : "sim"),
            engine_.live_snapshot().cash);
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
    draw_signin_modal();
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
        watchlist_.draw(&show_watchlist_,
                        [this](const std::string& sym) { chart_.show_symbol(sym); });
    if (show_backtest_)
        backtest_.draw(&show_backtest_, strat_mgr_.active_name(), sweep_.running,
                       [this](const std::string& sym, const std::string& ivl,
                              const std::string& rng, double cash) {
                           queue_backtest(sym, ivl, rng, cash);
                       });
    if (show_sweep_)
        sweep_panel_.draw(&show_sweep_, strat_mgr_.active_name(),
                          strat_mgr_.param_values(), sweep_,
                          [this](const SweepPanel::Request& rq) { queue_sweep(rq); },
                          [this] {
                              sweep_.running = false;
                              log_.add("sweep: cancelled");
                          });
    if (show_strategy_) strat_mgr_.draw(&show_strategy_);
    if (show_trade_)
        trade_.draw(&show_trade_, strat_mgr_.active_name(), alpaca_creds().has_value(),
                    !polygon_key().empty(),
                    [this](const TradePanel::StartOpts& opts) {
                        const std::vector<std::string>& syms = opts.symbols;
                        LiveConfig cfg;
                        cfg.symbols = syms;
                        cfg.initial_cash = opts.cash;
                        cfg.params = strat_mgr_.param_values();
                        cfg.bar_seconds = opts.bar_seconds;
                        cfg.risk = opts.risk;
                        // Real-time feed => spin the engine thread instead of
                        // sleeping; ticks are handled in ns, not after Sleep(5).
                        cfg.busy_spin = opts.data != TradePanel::DataFeed::Delayed;
                        // Optional core pinning (TT_PIN_ENGINE / TT_PIN_FEED =
                        // core index): kills scheduler-migration jitter.
                        if (const char* pin = std::getenv("TT_PIN_ENGINE"))
                            cfg.pin_core = std::atoi(pin);
                        if (opts.record) {
                            std::error_code ec;
                            std::filesystem::create_directories(sessions_dir(), ec);
                            char name[32];
                            const std::time_t now = std::time(nullptr);
                            std::tm tm{};
                            localtime_s(&tm, &now);
                            std::strftime(name, sizeof name, "%Y%m%d_%H%M%S.ttk", &tm);
                            cfg.capture_path = sessions_dir() + "\\" + name;
                        }
                        // Paper endpoints only for now; going live is a deliberate
                        // future step, not an env-var surprise.
                        std::unique_ptr<AlpacaBroker> broker;
                        std::unique_ptr<IbkrBroker> ibkr_broker;
                        const std::optional<Account> creds = alpaca_creds();
                        if (opts.broker == TradePanel::Broker::Alpaca && creds) {
                            AlpacaConfig ac;
                            ac.key_id = creds->key_id;
                            ac.secret = creds->secret;
                            ac.symbols = syms;
                            broker = std::make_unique<AlpacaBroker>(std::move(ac));
                            cfg.broker = broker.get();
                            log_.add("live: using Alpaca account '" + creds->name + "'");
                        } else if (opts.broker == TradePanel::Broker::Ibkr) {
                            IbkrConfig ic;
                            if (const char* gw = std::getenv("TT_IBKR_GATEWAY"))
                                ic.gateway_url = gw;
                            ic.symbols = syms;
                            ibkr_broker = std::make_unique<IbkrBroker>(std::move(ic));
                            cfg.broker = ibkr_broker.get();
                            log_.add("live: routing orders to IBKR gateway");
                        }
                        if (engine_.start_live(std::move(cfg),
                                               strat_mgr_.active_strategy(sma_))) {
                            // Previous session's thread was joined inside
                            // start_live, so replacing the old brokers is safe.
                            alpaca_ = std::move(broker);
                            ibkr_ = std::move(ibkr_broker);
                            alpaca_feed_.reset();   // previous session's feeds
                            polygon_feed_.reset();
                            ibkr_feed_.reset();
                            rt_feed_active_.store(false, std::memory_order_relaxed);
                            const auto sink = [this](const EngineEvent& ev) {
                                return engine_.push_feed_event(ev);
                            };
                            if (opts.data == TradePanel::DataFeed::AlpacaIex && creds) {
                                AlpacaFeedConfig fc;
                                fc.key_id = creds->key_id;
                                fc.secret = creds->secret;
                                fc.symbols = syms;
                                fc.busy_poll = std::getenv("TT_FEED_SPIN") != nullptr;
                                if (const char* pin = std::getenv("TT_PIN_FEED"))
                                    fc.pin_core = std::atoi(pin);
                                fc.bar_seconds = opts.bar_seconds;
                                alpaca_feed_ =
                                    std::make_unique<AlpacaFeed>(std::move(fc), sink);
                                alpaca_feed_->start();
                                rt_feed_active_.store(true, std::memory_order_relaxed);
                            } else if (opts.data == TradePanel::DataFeed::Polygon &&
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
                                pc.bar_seconds = opts.bar_seconds;
                                polygon_feed_ =
                                    std::make_unique<PolygonFeed>(std::move(pc), sink);
                                polygon_feed_->start();
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
                                fc.bar_seconds = opts.bar_seconds;
                                if (const char* pin = std::getenv("TT_PIN_FEED"))
                                    fc.pin_core = std::atoi(pin);
                                ibkr_feed_ =
                                    std::make_unique<IbkrFeed>(std::move(fc), sink);
                                ibkr_feed_->start();
                                rt_feed_active_.store(true, std::memory_order_relaxed);
                            }
                            for (const std::string& sym : syms)
                                watchlist_.ensure(sym);  // quote subscription feeds the engine
                            std::string joined;
                            for (const std::string& sym : syms)
                                joined += (joined.empty() ? "" : ",") + sym;
                            log_.add("live: session queued for " + joined + " (" +
                                     strat_mgr_.active_name() + ")");
                        } else {
                            log_.add("live: cannot start (engine busy)");
                        }
                    },
                    [this](const std::string& path) {
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
                        cfg.initial_cash = trade_.cash();
                        cfg.params = strat_mgr_.param_values();
                        if (engine_.start_replay(std::move(cfg),
                                                 strat_mgr_.active_strategy(sma_)))
                            log_.add("replay: running " + path + " (" +
                                     strat_mgr_.active_name() + ")");
                        else
                            log_.add("replay: engine busy, try again");
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
            cfg.params = strat_mgr_.param_values();
            cfg.bar_seconds = 2;
            engine_.start_live(std::move(cfg), strat_mgr_.active_strategy(sma_));
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

void App::draw_account_menu() {
    if (!ImGui::BeginMenu("Account")) return;
    for (const char* prov : {"alpaca", "polygon"}) {
        const std::string act = accounts_.active_name(prov);
        ImGui::TextDisabled("%s: %s", prov, act.empty() ? "(not signed in)" : act.c_str());
    }
    ImGui::TextDisabled("ibkr: gateway browser login");
    ImGui::Separator();

    if (ImGui::MenuItem("Sign In...")) signin_.request_open = true;

    if (ImGui::BeginMenu("Switch", !accounts_.list().empty())) {
        for (const auto& e : accounts_.list()) {
            const bool is_active = accounts_.active_name(e.provider) == e.name;
            const std::string label = e.name + "  (" + e.provider + ")";
            if (ImGui::MenuItem(label.c_str(), nullptr, is_active) && !is_active) {
                if (accounts_.get(e.name)) {
                    accounts_.set_active(e.name);
                    log_.add("account: switched " + e.provider + " to '" + e.name + "'");
                } else {
                    log_.add("account: cannot decrypt '" + e.name +
                             "' (saved by another Windows user?)");
                }
            }
        }
        ImGui::EndMenu();
    }

    const bool any_active =
        accounts_.signed_in("alpaca") || accounts_.signed_in("polygon");
    if (ImGui::BeginMenu("Sign Out", any_active)) {
        for (const char* prov : {"alpaca", "polygon"}) {
            const std::string act = accounts_.active_name(prov);
            if (!act.empty() && ImGui::MenuItem((std::string(prov) + ": " + act).c_str())) {
                accounts_.sign_out(prov);
                log_.add(std::string("account: signed out of ") + prov);
            }
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
    if (has("KILL SWITCH") || has("RISK HALT"))
        alerts_.notify(AlertNotifier::Critical, l);
    else if (has("rejected") || has("stream lost") || has("auth failed") ||
             has("(drops!)"))
        alerts_.notify(AlertNotifier::Warning, l);
    else if (has("live: fill"))
        alerts_.notify(AlertNotifier::Info, l);
}

void App::draw_signin_modal() {
    if (signin_.request_open) {
        signin_.request_open = false;
        signin_.status.store(0);
        {
            std::lock_guard lock(signin_.mu);
            signin_.detail.clear();
        }
        ImGui::OpenPopup("Sign In to Alpaca");
    }
    if (!ImGui::BeginPopupModal("Sign In to Alpaca", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const bool is_alpaca = signin_.provider == 0;
    const bool is_polygon = signin_.provider == 1;
    auto make_account = [&] {
        Account a;
        a.name = signin_.name[0] ? signin_.name : "default";
        a.provider = is_polygon ? "polygon" : "alpaca";
        a.key_id = signin_.key;
        a.secret = is_polygon ? "" : signin_.secret;
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
    static constexpr const char* kProviders[] = {"Alpaca", "Polygon", "IBKR"};
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Provider", &signin_.provider, kProviders, IM_ARRAYSIZE(kProviders));
    if (signin_.provider == 2) {
        ImGui::EndDisabled();
        // Nothing to store: the gateway holds the brokerage session itself.
        ImGui::TextWrapped("IBKR credentials are never stored here. Run the Client "
                           "Portal Gateway and log in via your browser (default "
                           "https://localhost:5000); the IBKR broker connects to "
                           "that session automatically.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("Name", signin_.name, sizeof signin_.name);
    ImGui::SetItemTooltip("Label shown in the Account menu (e.g. \"paper\")");
    ImGui::SetNextItemWidth(280);
    ImGui::InputText(is_polygon ? "API key" : "API key ID", signin_.key,
                     sizeof signin_.key, is_polygon ? ImGuiInputTextFlags_Password : 0);
    if (is_alpaca) {
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("API secret", signin_.secret, sizeof signin_.secret,
                         ImGuiInputTextFlags_Password);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Stored encrypted (Windows DPAPI, this user only).");

    if (busy) {
        ImGui::TextDisabled("Verifying...");
    } else if (status == 3) {
        std::lock_guard lock(signin_.mu);
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "Failed: %s",
                           signin_.detail.c_str());
    }

    const bool have_input = signin_.key[0] && (is_polygon || signin_.secret[0]);
    ImGui::BeginDisabled(busy || !have_input);
    if (ImGui::Button("Verify & Sign In")) {
        if (signin_.worker.joinable()) signin_.worker.join();
        signin_.status.store(1);
        const std::string key = signin_.key, secret = signin_.secret;
        const bool polygon = is_polygon;
        signin_.worker = std::thread([this, key, secret, polygon] {
            std::string detail;
            const bool ok =
                polygon
                    ? polygon_verify_key(PolygonFeedConfig{}.rest_url, key, detail)
                    : alpaca_verify_account(AlpacaConfig{}.rest_url, key, secret, detail);
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
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Watchlist", nullptr, &show_watchlist_);
        ImGui::MenuItem("Backtest", nullptr, &show_backtest_);
        ImGui::MenuItem("Optimizer", nullptr, &show_sweep_);
        ImGui::MenuItem("Strategy", nullptr, &show_strategy_);
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

    // Right-aligned feed health indicator.
    const bool up = ipc_.connected();
    const char* label = up ? "FEED UP" : "FEED DOWN";
    const float w = ImGui::CalcTextSize(label).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextColored(up ? ImVec4(0.25f, 0.85f, 0.45f, 1.0f)
                          : ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "%s", label);
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
