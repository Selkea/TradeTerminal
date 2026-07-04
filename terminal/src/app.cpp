#include "app.h"
#include "dev_paths.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout)
#include "implot.h"

#include <cstdlib>
#include <filesystem>

namespace tt::ui {

namespace {
std::string strategies_out_dir() {
    const char* base = std::getenv("LOCALAPPDATA");
    return (std::filesystem::path(base ? base : ".") / "TradeTerminal" / "strategies")
        .string();
}
std::string gxx_path() {
    const char* env = std::getenv("TT_GXX");
    return env ? env : TT_GXX_DEFAULT;
}
} // namespace

App::App(std::string python_cmd, std::string service_dir)
    : ipc_(std::move(python_cmd), std::move(service_dir)),
      host_(gxx_path(), std::string(TT_REPO_ROOT) + "/sdk/include", strategies_out_dir()),
      chart_(ipc_, series_),
      watchlist_(ipc_, quotes_),
      backtest_(engine_),
      strat_mgr_(host_, engine_, std::string(TT_REPO_ROOT) + "/strategies") {
    net::IpcClient::Callbacks cbs;
    cbs.on_log = [this](std::string line) { log_.add(std::move(line)); };
    cbs.on_tick = [this](const std::string& sym, const Quote& q) { quotes_.set(sym, q); };
    cbs.on_error = [this](uint32_t id, std::string code, std::string msg) {
        log_.add("feed error (req " + std::to_string(id) + ") " + code + ": " + msg);
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_.active = false;  // data never arrives; don't wedge the panel
    };
    cbs.on_candles = [this](net::CandleBatch&& b) {
        log_.add("candles: " + b.symbol + " " + b.interval + " x" +
                 std::to_string(b.candles.size()) + (b.cached ? " (cache)" : ""));
        start_pending_backtest(b);
        series_.put(b.symbol, b.interval, std::move(b.candles), b.cached);
    };
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

App::~App() { ipc_.stop(); }

void App::draw() {
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    // Surface engine/strategy log lines in the console.
    std::string line;
    while (engine_.pop_log(line)) log_.add(std::move(line));

    // TT_AUTORUN_BACKTEST=1: fire one AAPL backtest as soon as the feed is up
    // (headless end-to-end verification of the UI -> data -> engine path).
    if (!autorun_bt_done_ && ipc_.connected() && std::getenv("TT_AUTORUN_BACKTEST")) {
        autorun_bt_done_ = true;
        queue_backtest("AAPL", "1d", "2y", 100'000.0);
        log_.add("autorun: queued AAPL 1d 2y backtest (" + strat_mgr_.active_name() + ")");
    }

    draw_menu_bar();
    if (show_chart_) chart_.draw(&show_chart_);
    if (show_watchlist_)
        watchlist_.draw(&show_watchlist_,
                        [this](const std::string& sym) { chart_.show_symbol(sym); });
    if (show_backtest_)
        backtest_.draw(&show_backtest_, strat_mgr_.active_name(),
                       [this](const std::string& sym, const std::string& ivl,
                              const std::string& rng, double cash) {
                           queue_backtest(sym, ivl, rng, cash);
                       });
    if (show_strategy_) strat_mgr_.draw(&show_strategy_);
    if (show_log_) log_.draw("Log Console", &show_log_);
    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Watchlist", nullptr, &show_watchlist_);
        ImGui::MenuItem("Backtest", nullptr, &show_backtest_);
        ImGui::MenuItem("Strategy", nullptr, &show_strategy_);
        ImGui::MenuItem("Log Console", nullptr, &show_log_);
        ImGui::EndMenu();
    }
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
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
