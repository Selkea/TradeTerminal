#pragma once

#include "market_data.h"
#include "net/market_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tt::ui {

// A fill to mark on the price plot (triangle up = buy, down = sell).
struct FillMarker {
    double ts_sec;
    double price;
    bool buy;
};

class ChartPanel {
public:
    ChartPanel(net::IMarketData& ipc, SeriesStore& store) : ipc_(ipc), store_(store) {}

    // fills: markers for the currently charted symbol (App matches symbols).
    void draw(bool* open, const std::vector<FillMarker>& fills = {});
    void show_symbol(const std::string& symbol);  // e.g. watchlist row clicked

    // Session persistence.
    std::string symbol() const { return sym_; }
    int interval_idx() const { return interval_idx_; }
    int range_idx() const { return range_idx_; }
    void restore(const std::string& sym, int ivl_idx, int rng_idx);

private:
    void request();
    void rebuild_plot_arrays(const SeriesStore::Series& s);

    net::IMarketData& ipc_;
    SeriesStore& store_;

    char sym_[16] = "AAPL";
    int interval_idx_ = 1;
    int range_idx_ = 1;
    bool requested_once_ = false;
    uint64_t seen_rev_ = 0;
    uint64_t seen_conn_gen_ = 0;
    bool fit_next_ = false;
    bool from_cache_ = false;

    std::vector<double> xs_, opens_, highs_, lows_, closes_, vols_;
    double width_sec_ = 0.0;
};

} // namespace tt::ui
