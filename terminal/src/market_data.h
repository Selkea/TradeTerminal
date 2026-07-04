#pragma once
// UI-side market data stores. Written by the IPC reader thread, read by the
// UI thread each frame — everything is mutex-guarded, with a revision counter
// so the UI only copies candle series out when they actually changed.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tt {

struct Candle {
    int64_t ts;  // epoch seconds (wire format), converted to ns at engine ingest
    double open, high, low, close, volume;
};
static_assert(sizeof(Candle) == 48, "must match CandleRecord wire layout");

struct Quote {
    double price = 0.0;
    int64_t ts_ms = 0;
    double day_volume = 0.0;
};

class SeriesStore {
public:
    struct Series {
        std::vector<Candle> candles;
        bool cached = false;
        uint64_t revision = 0;
    };

    void put(const std::string& symbol, const std::string& interval,
             std::vector<Candle> candles, bool cached) {
        std::lock_guard lock(mu_);
        Series& s = series_[key(symbol, interval)];
        s.candles = std::move(candles);
        s.cached = cached;
        s.revision = ++revision_counter_;
    }

    // Copies the series out only if its revision advanced past `seen`.
    bool copy_if_newer(const std::string& symbol, const std::string& interval,
                      uint64_t& seen, Series& out) const {
        std::lock_guard lock(mu_);
        auto it = series_.find(key(symbol, interval));
        if (it == series_.end() || it->second.revision <= seen) return false;
        out = it->second;
        seen = it->second.revision;
        return true;
    }

private:
    static std::string key(const std::string& symbol, const std::string& interval) {
        return symbol + "|" + interval;
    }
    mutable std::mutex mu_;
    std::map<std::string, Series> series_;
    uint64_t revision_counter_ = 0;
};

class QuoteBook {
public:
    void set(const std::string& symbol, const Quote& q) {
        std::lock_guard lock(mu_);
        quotes_[symbol] = q;
    }
    bool get(const std::string& symbol, Quote& out) const {
        std::lock_guard lock(mu_);
        auto it = quotes_.find(symbol);
        if (it == quotes_.end()) return false;
        out = it->second;
        return true;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Quote> quotes_;
};

} // namespace tt
