#pragma once

#include "engine/engine.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tt::ui {

class BacktestPanel {
public:
    // strategy source ("" = built-in), symbol, interval, range, initial
    // cash — params come from the Strategy Manager.
    using RunFn = std::function<void(const std::string&, const std::string&,
                                     const std::string&, const std::string&,
                                     double)>;

    explicit BacktestPanel(Engine& eng) : eng_(eng) {}
    // sources/active_key: the strategy dropdown ("" entry = built-in SMA);
    // loaded_fresh: whether a pick would run as-is or build first;
    // activating: a build/load kicked off by Run is still in flight.
    // suppress_result: a parameter sweep owns the engine's results right now.
    void draw(bool* open, const std::vector<std::string>& sources,
              const std::string& active_key,
              const std::function<bool(const std::string&)>& loaded_fresh,
              bool activating, bool suppress_result, const RunFn& run);

    double cash() const { return cash_; }
    void set_cash(double c) { cash_ = c; }
    // Watchlist click-through: point the next run at this symbol.
    void set_symbol(const std::string& sym);
    // Last finished result, or null (chart overlays fills for its symbol).
    const BacktestResult* result() const { return has_res_ ? &res_ : nullptr; }

private:
    void draw_results();
    void export_csv();

    Engine& eng_;
    char sym_[16] = "AAPL";
    std::string strat_sel_;      // dropdown pick, by basename; "" = built-in
    bool strat_init_ = false;    // first draw adopts whatever is loaded
    int interval_idx_ = 6;       // "1d" in kIntervals
    int range_idx_ = 3;
    double cash_ = 100'000.0;
    BacktestResult res_;
    bool has_res_ = false;
    std::string last_export_;
};

} // namespace tt::ui
