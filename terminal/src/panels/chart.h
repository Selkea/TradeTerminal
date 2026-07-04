#pragma once

#include "market_data.h"
#include "net/ipc_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tt::ui {

class ChartPanel {
public:
    ChartPanel(net::IpcClient& ipc, SeriesStore& store) : ipc_(ipc), store_(store) {}

    void draw(bool* open);
    void show_symbol(const std::string& symbol);  // e.g. watchlist row clicked

private:
    void request();
    void rebuild_plot_arrays(const SeriesStore::Series& s);

    net::IpcClient& ipc_;
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
