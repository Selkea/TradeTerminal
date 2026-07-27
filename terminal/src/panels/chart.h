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
    // live_price/live_ts_sec: the charted symbol's latest live price and tick
    // time (0 = none). When present and intraday, the newest bar is refined /
    // extended in real time without re-fetching (see approach B).
    void draw(bool* open, const std::vector<FillMarker>& fills = {},
              double live_price = 0.0, double live_ts_sec = 0.0);
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
    int interval_idx_ = 2;   // default "5m" (index shifts if kIntervals changes)
    int range_idx_ = 6;   // default "5d" (index shifts if kRanges changes)
    bool requested_once_ = false;
    uint64_t seen_rev_ = 0;
    uint64_t seen_conn_gen_ = 0;
    bool fit_next_ = false;
    bool from_cache_ = false;

    std::vector<double> xs_, opens_, highs_, lows_, closes_, vols_;
    std::vector<double> up_, dn_;   // close split into rising/falling segments
    double width_sec_ = 0.0;
    bool follow_live_ = true;   // keep the newest live bar in view (scroll with it)
    double view_span_ = 0.0;    // width of the fetched window; the follow window size
    // Live-tail state: ts of the last fetched (historical) bar, and the bucket of
    // the live bar currently appended past it. Reset on every store rebuild.
    double hist_last_ts_ = 0.0;
    double live_tail_bucket_ = 0.0;
};

} // namespace tt::ui
