#include "net/tws_data.h"

#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <unordered_map>

namespace tt::net {

namespace {

// Streaming quote tickerIds live far above the candle request ids (which are
// the public next_id_ values) so the two spaces can never collide.
constexpr int kQuoteBase = 500'000;

// TickType ids on the reqMktData stream (live + delayed variants).
constexpr int kTickLast = 4, kTickDelayedLast = 68;
constexpr int kTickVolume = 8, kTickDelayedVolume = 74;

const char* tws_bar_size(const std::string& interval) {
    if (interval == "1m") return "1 min";
    if (interval == "2m") return "2 mins";
    if (interval == "5m") return "5 mins";
    if (interval == "15m") return "15 mins";
    if (interval == "30m") return "30 mins";
    if (interval == "1h") return "1 hour";
    if (interval == "1d") return "1 day";
    return nullptr;
}

// Duration ladder, smallest to largest; index doubles as a comparable rank.
struct Dur {
    const char* range;
    const char* dur;
};
constexpr Dur kDurs[] = {{"1d", "1 D"}, {"5d", "5 D"},  {"1mo", "1 M"},
                         {"6mo", "6 M"}, {"1y", "1 Y"},  {"2y", "2 Y"},
                         {"5y", "5 Y"},  {"max", "15 Y"}};

int dur_idx(const std::string& range) {
    for (int i = 0; i < static_cast<int>(std::size(kDurs)); ++i)
        if (range == kDurs[i].range) return i;
    return -1;
}

// IB caps how much history one request may span per bar size; clamp so a
// "1m x 5y" chart still shows the most recent stretch instead of erroring.
int max_dur_idx(const std::string& interval) {
    if (interval == "1m" || interval == "2m") return 2;    // <= 1 M
    if (interval == "5m" || interval == "15m" || interval == "30m")
        return 3;                                          // <= 6 M
    if (interval == "1h") return 5;                        // <= 2 Y
    return static_cast<int>(std::size(kDurs)) - 1;         // daily: anything
}

// formatDate=2 gives epoch seconds for intraday bars but "YYYYMMDD" for
// daily bars; normalize both to epoch seconds (daily at UTC midnight).
int64_t parse_bar_time(const std::string& t) {
    if (t.size() == 8 && t.find_first_not_of("0123456789") == std::string::npos) {
        std::tm tm{};
        tm.tm_year = std::stoi(t.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(t.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(t.substr(6, 2));
#ifdef _WIN32
        return static_cast<int64_t>(_mkgmtime(&tm));
#else
        return static_cast<int64_t>(timegm(&tm));
#endif
    }
    try {
        return std::stoll(t);
    } catch (...) {
        return 0;
    }
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct TwsData::Io final : DefaultEWrapper {
    TwsData& d;
    EReaderOSSignal signal;
    std::unique_ptr<EClientSocket> client;
    std::unique_ptr<EReader> reader;

    // In-flight candle fetches: TWS reqId == the public request id.
    struct Pending {
        std::string symbol, interval;
        std::vector<Candle> candles;
    };
    std::unordered_map<int, Pending> hist;

    // Active quote streams: symbol -> tickerId + last known quote state.
    struct Stream {
        int req_id = 0;
        Quote q;
    };
    std::unordered_map<std::string, Stream> streams;
    int next_quote_req = kQuoteBase;

    explicit Io(TwsData& data) : d(data), signal(1000) {}

    bool connect_gateway() {
        client = std::make_unique<EClientSocket>(this, &signal);
        if (!client->eConnect(d.host_.c_str(), d.port_, d.client_id_)) {
            client.reset();
            return false;
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        d.log("connecting to IB Gateway at " + d.host_ + ":" +
              std::to_string(d.port_));
        return true;
    }

    void drop_connection() {
        d.connected_.store(false, std::memory_order_release);
        if (client) client->eDisconnect();
        reader.reset();
        client.reset();
        // In-flight history requests will never answer; unblock the waiters.
        for (auto& [id, p] : hist)
            if (d.cbs_.on_error)
                d.cbs_.on_error(static_cast<uint32_t>(id), "tws",
                                "connection lost fetching " + p.symbol);
        hist.clear();
        streams.clear();   // re-established on reconnect via want_dirty_
    }

    static Contract stock(const std::string& sym) {
        Contract c;
        c.symbol = sym;
        c.secType = "STK";
        c.exchange = "SMART";
        c.currency = "USD";
        return c;
    }

    // Drain UI-thread requests; only when a session is up so nothing is lost.
    void pump_requests() {
        if (!client || !d.connected_.load(std::memory_order_acquire)) return;
        std::vector<CandleReq> reqs;
        std::vector<std::string> want;
        bool dirty = false;
        {
            std::lock_guard lock(d.mu_);
            reqs.swap(d.reqs_);
            if (d.want_dirty_) {
                want = d.want_syms_;
                d.want_dirty_ = false;
                dirty = true;
            }
        }
        for (const CandleReq& r : reqs) {
            const char* bar = tws_bar_size(r.interval);
            int di = dur_idx(r.range);
            if (!bar || di < 0) {
                if (d.cbs_.on_error)
                    d.cbs_.on_error(r.id, "tws",
                                    "cannot fetch " + r.symbol + " " + r.interval);
                continue;
            }
            const int cap = max_dur_idx(r.interval);
            if (di > cap) {
                d.log(r.symbol + ": " + r.range + " of " + r.interval +
                      " bars exceeds IB's history window - clamped to " +
                      kDurs[cap].dur);
                di = cap;
            }
            hist[static_cast<int>(r.id)] = {r.symbol, r.interval, {}};
            client->reqHistoricalData(static_cast<TickerId>(r.id), stock(r.symbol),
                                      "", kDurs[di].dur, bar, "TRADES",
                                      /*useRTH=*/1, /*formatDate=*/2,
                                      /*keepUpToDate=*/false, TagValueListSPtr());
        }
        if (dirty) {
            for (auto it = streams.begin(); it != streams.end();) {
                if (std::find(want.begin(), want.end(), it->first) == want.end()) {
                    client->cancelMktData(it->second.req_id);
                    it = streams.erase(it);
                } else {
                    ++it;
                }
            }
            for (const std::string& s : want) {
                if (streams.count(s)) continue;
                Stream st;
                st.req_id = next_quote_req++;
                streams.emplace(s, st);
                client->reqMktData(st.req_id, stock(s), "", false, false,
                                   TagValueListSPtr());
            }
        }
    }

    // ---- EWrapper -----------------------------------------------------------
    void nextValidId(OrderId) override {
        // Delayed data is fine when the account has no real-time subscription
        // (type 3 = delayed-if-needed); applies to history + streams alike.
        if (client) client->reqMarketDataType(3);
        {
            std::lock_guard lock(d.mu_);
            d.want_dirty_ = true;   // fresh session: re-establish every stream
        }
        if (!d.connected_.exchange(true, std::memory_order_acq_rel)) {
            d.conn_gen_.fetch_add(1, std::memory_order_relaxed);
            d.log("session up (IB Gateway " + d.host_ + ":" +
                  std::to_string(d.port_) + ")");
        }
    }

    void managedAccounts(const std::string& list) override {
        std::vector<std::string> all;
        std::string cur;
        for (const char ch : list + ",") {
            if (ch == ',') {
                if (!cur.empty()) all.push_back(cur);
                cur.clear();
            } else if (ch != ' ') {
                cur += ch;
            }
        }
        // Paper account codes start with 'D' (DU/DF prefixes).
        AccountKind kind = AccountKind::Unknown;
        if (!all.empty())
            kind = all[0].front() == 'D' ? AccountKind::Paper : AccountKind::Live;
        d.account_kind_.store(kind, std::memory_order_release);
        std::lock_guard lock(d.mu_);
        d.account_ = all.empty() ? "" : all[0];
        d.accounts_ = std::move(all);
    }

    void connectionClosed() override {
        d.connected_.store(false, std::memory_order_release);
        d.account_kind_.store(AccountKind::Unknown, std::memory_order_release);
        d.log("connection closed");
    }

    void error(int id, int errorCode, const std::string& errorString,
               const std::string&) override {
        if (errorCode >= 2100 && errorCode <= 2170) return;   // farm status noise
        const auto it = hist.find(id);
        if (it != hist.end()) {
            if (d.cbs_.on_error)
                d.cbs_.on_error(static_cast<uint32_t>(id), "tws",
                                it->second.symbol + ": " + errorString);
            hist.erase(it);
            return;
        }
        d.log("error " + std::to_string(errorCode) + " (id " + std::to_string(id) +
              "): " + errorString);
    }

    void historicalData(TickerId reqId, const ::Bar& bar) override {
        const auto it = hist.find(static_cast<int>(reqId));
        if (it == hist.end()) return;
        Candle c;
        c.ts = parse_bar_time(bar.time);
        c.open = bar.open;
        c.high = bar.high;
        c.low = bar.low;
        c.close = bar.close;
        c.volume = DecimalFunctions::decimalToDouble(bar.volume);
        if (c.volume < 0) c.volume = 0;   // -1 = not reported for this bar
        it->second.candles.push_back(c);
    }

    void historicalDataEnd(int reqId, const std::string&,
                           const std::string&) override {
        const auto it = hist.find(reqId);
        if (it == hist.end()) return;
        CandleBatch b;
        b.id = static_cast<uint32_t>(reqId);
        b.symbol = it->second.symbol;
        b.interval = it->second.interval;
        b.candles = std::move(it->second.candles);
        hist.erase(it);
        if (d.cbs_.on_candles) d.cbs_.on_candles(std::move(b));
    }

    void tickPrice(TickerId tickerId, TickType field, double price,
                   const TickAttrib&) override {
        const int ft = static_cast<int>(field);
        if ((ft != kTickLast && ft != kTickDelayedLast) || price <= 0.0) return;
        for (auto& [sym, st] : streams) {
            if (st.req_id != static_cast<int>(tickerId)) continue;
            st.q.price = price;
            st.q.ts_ms = now_ms();
            if (d.cbs_.on_tick) d.cbs_.on_tick(sym, st.q);
            break;
        }
    }

    void tickSize(TickerId tickerId, TickType field, Decimal size) override {
        const int ft = static_cast<int>(field);
        if (ft != kTickVolume && ft != kTickDelayedVolume) return;
        for (auto& [sym, st] : streams) {
            if (st.req_id != static_cast<int>(tickerId)) continue;
            st.q.day_volume = DecimalFunctions::decimalToDouble(size);
            break;
        }
    }
};

// ---- adapter -------------------------------------------------------------

TwsData::~TwsData() { stop(); }

void TwsData::set_endpoint(std::string host, int port, int client_id) {
    host_ = std::move(host);
    port_ = port;
    client_id_ = client_id;
}

void TwsData::start(Callbacks cbs) {
    if (io_thread_.joinable()) return;
    cbs_ = std::move(cbs);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this] { io_loop(); });
}

void TwsData::stop() {
    running_.store(false, std::memory_order_release);
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

uint32_t TwsData::request_candles(const std::string& symbol,
                                  const std::string& interval,
                                  const std::string& range) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mu_);
        reqs_.push_back({id, symbol, interval, range});
    }
    wake();
    return id;
}

uint32_t TwsData::subscribe_quotes(const std::vector<std::string>& symbols,
                                   int /*poll_s*/) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mu_);
        want_syms_ = symbols;
        want_dirty_ = true;
        quote_sub_ = id;
    }
    wake();
    return id;
}

void TwsData::unsubscribe(uint32_t sub_id) {
    std::lock_guard lock(mu_);
    if (sub_id != quote_sub_) return;   // superseded subscriptions are gone
    want_syms_.clear();
    want_dirty_ = true;
}

std::string TwsData::account() const {
    std::lock_guard lock(mu_);
    return account_;
}

std::vector<std::string> TwsData::accounts() const {
    std::lock_guard lock(mu_);
    return accounts_;
}

void TwsData::log(std::string msg) {
    if (cbs_.on_log) cbs_.on_log("tws-data: " + std::move(msg));
}

void TwsData::wake() {
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
}

void TwsData::io_loop() {
    Io io(*this);
    wake_.store(&io.signal, std::memory_order_release);

    auto last_connect = std::chrono::steady_clock::time_point{};
    int64_t last_nag_ms = 0;
    while (running_.load(std::memory_order_acquire)) {
        if (!io.client || !io.client->isConnected()) {
            io.drop_connection();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_connect >= std::chrono::seconds(3)) {
                last_connect = now;
                if (!io.connect_gateway()) {
                    const int64_t ms = now_ms();
                    if (ms - last_nag_ms > 30'000) {
                        last_nag_ms = ms;
                        log("cannot reach IB Gateway at " + host_ + ":" +
                            std::to_string(port_) +
                            " - run scripts\\Start-IbGateway.ps1");
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            io.signal.waitForSignal();
            if (io.reader) io.reader->processMsgs();
            io.pump_requests();
        }
    }

    wake_.store(nullptr, std::memory_order_release);
    io.drop_connection();
}

} // namespace tt::net
