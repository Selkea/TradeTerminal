#pragma once

#include "market_data.h"
#include "net/gateway_data.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace tt::ui {

class WatchlistPanel {
public:
    WatchlistPanel(net::GatewayData& ipc, QuoteBook& quotes) : ipc_(ipc), quotes_(quotes) {}

    // on_select fires when the user clicks a row (chart follows).
    void draw(bool* open, const std::function<void(const std::string&)>& on_select);

    const std::vector<std::string>& symbols() const { return symbols_; }
    void set_symbols(std::vector<std::string> v) {
        symbols_ = std::move(v);
        dirty_ = true;
    }

    // Guarantee the symbol is subscribed (live sessions need its quotes).
    void ensure(const std::string& symbol) {
        if (std::find(symbols_.begin(), symbols_.end(), symbol) == symbols_.end()) {
            symbols_.push_back(symbol);
            dirty_ = true;
        }
    }

private:
    void resubscribe();

    net::GatewayData& ipc_;
    QuoteBook& quotes_;

    std::vector<std::string> symbols_ = {"AAPL", "MSFT", "SPY"};
    char input_[16] = "";
    bool dirty_ = true;
    uint64_t seen_conn_gen_ = 0;
};

} // namespace tt::ui
