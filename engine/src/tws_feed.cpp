#include "engine/tws_feed.h"

#include "engine/clock.h"

#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

#include <chrono>
#include <cstdio>

namespace tt {

namespace {
// reqId spaces: tick-by-tick Last, tick-by-tick BidAsk, and the streaming
// reqMktData fallback. Symbol id = reqId - base.
constexpr int kLastBase = 1000;
constexpr int kBidAskBase = 2000;
constexpr int kMktDataBase = 3000;

// TickType ids we care about on the reqMktData fallback path (live + delayed).
constexpr int kTickBid = 1, kTickAsk = 2, kTickLast = 4;
constexpr int kTickDelayedBid = 66, kTickDelayedAsk = 67, kTickDelayedLast = 68;

// Steady-clock milliseconds — the watchdog and connect path share this timebase.
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

struct TwsFeed::Io final : DefaultEWrapper {
    TwsFeed& f;
    EReaderOSSignal signal;
    std::unique_ptr<EClientSocket> client;
    std::unique_ptr<EReader> reader;
    bool subscribed = false;
    // Handshake rejected (e.g. paper disclaimer not accepted yet): tear down
    // and retry from io_loop — destroying the reader inside a callback is unsafe.
    bool reset_conn = false;

    struct BidAsk {
        double bid = 0.0, ask = 0.0;
    };
    std::vector<BidAsk> quotes;

    explicit Io(TwsFeed& feed) : f(feed), signal(1000) {
        quotes.assign(f.cfg_.symbols.size(), {});
    }

    bool connect_gateway() {
        client = std::make_unique<EClientSocket>(this, &signal);
        // Publish the in-flight socket so the watchdog can abort a handshake that
        // never completes (see TwsFeed::watchdog_loop). Cleared under the lock
        // once eConnect returns.
        {
            std::lock_guard<std::mutex> lk(f.conn_mu_);
            f.connecting_ = client.get();
            f.connect_started_ms_.store(now_ms(), std::memory_order_release);
        }
        const bool ok = client->eConnect(f.cfg_.host.c_str(), f.cfg_.port,
                                         f.cfg_.client_id);
        {
            std::lock_guard<std::mutex> lk(f.conn_mu_);
            f.connecting_ = nullptr;
            f.connect_started_ms_.store(0, std::memory_order_release);
        }
        if (!ok) {   // includes a watchdog-forced abort — retried after backoff
            client.reset();
            return false;
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        f.log("connecting to IB Gateway at " + f.cfg_.host + ":" +
              std::to_string(f.cfg_.port));
        return true;
    }

    void drop_connection() {
        f.connected_.store(false, std::memory_order_release);
        subscribed = false;
        if (client) client->eDisconnect();
        reader.reset();
        client.reset();
    }

    Contract make_contract(size_t i) const {
        Contract c;
        c.symbol = f.cfg_.symbols[i];
        c.secType = "STK";
        c.exchange = "SMART";
        c.currency = "USD";
        return c;
    }

    void subscribe_all() {
        // Delayed data is acceptable on the fallback path when the account has
        // no real-time subscription (type 3 = delayed-if-needed).
        client->reqMarketDataType(3);
        for (size_t i = 0; i < f.cfg_.symbols.size(); ++i) {
            const Contract c = make_contract(i);
            client->reqTickByTickData(kLastBase + static_cast<int>(i + 1), c, "Last",
                                      0, false);
            client->reqTickByTickData(kBidAskBase + static_cast<int>(i + 1), c,
                                      "BidAsk", 0, false);
        }
        subscribed = true;
        std::string joined;
        for (const std::string& s : f.cfg_.symbols)
            joined += (joined.empty() ? "" : ",") + s;
        f.log("streaming " + joined + " (tick-by-tick)");
    }

    void emit_tick(uint32_t sid, int64_t ts_ns, double price, double size) {
        if (sid < 1 || sid > quotes.size() || price <= 0.0) return;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::Tick);
        ev.symbol_id = sid;
        ev.ts_event_ns = ts_ns;
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.tick.price = price;
        ev.u.tick.size = size;
        ev.u.tick.bid = quotes[sid - 1].bid;
        ev.u.tick.ask = quotes[sid - 1].ask;
        if (!f.sink_(ev)) f.dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- EWrapper -----------------------------------------------------------
    void nextValidId(OrderId) override {
        f.connected_.store(true, std::memory_order_release);
        if (!subscribed && client) subscribe_all();
    }

    void connectionClosed() override {
        f.connected_.store(false, std::memory_order_release);
        f.log("connection closed");
    }

    void error(int id, int errorCode, const std::string& errorString,
               const std::string&) override {
        if (errorCode >= 2100 && errorCode <= 2170) return;   // farm status noise
        if (errorCode == 10141) {   // paper disclaimer dialog not clicked yet (IBC lags login)
            f.log("gateway still accepting the paper disclaimer - retrying shortly");
            reset_conn = true;
            return;
        }
        // Tick-by-tick refused (no subscription / stream limit): fall back to
        // streaming market data for that symbol. BidAsk refusals just leave
        // bid/ask at 0 if the Last fallback already runs.
        if (id >= kLastBase && id < kLastBase + 999) {
            const int sid = id - kLastBase;
            f.log("tick-by-tick refused for #" + std::to_string(sid) + " (" +
                  std::to_string(errorCode) + "): " + errorString +
                  " - falling back to streaming snapshots");
            if (client && sid >= 1 && sid <= static_cast<int>(f.cfg_.symbols.size()))
                client->reqMktData(kMktDataBase + sid,
                                   make_contract(static_cast<size_t>(sid - 1)), "",
                                   false, false, TagValueListSPtr());
            return;
        }
        if (id >= kBidAskBase && id < kBidAskBase + 999) return;   // quotes optional
        f.log("error " + std::to_string(errorCode) + " (id " + std::to_string(id) +
              "): " + errorString);
    }

    void tickByTickAllLast(int reqId, int /*tickType*/, time_t time, double price,
                           Decimal size, const TickAttribLast&, const std::string&,
                           const std::string&) override {
        emit_tick(static_cast<uint32_t>(reqId - kLastBase),
                  static_cast<int64_t>(time) * 1'000'000'000, price,
                  DecimalFunctions::decimalToDouble(size));
    }

    void tickByTickBidAsk(int reqId, time_t /*time*/, double bidPrice, double askPrice,
                          Decimal, Decimal, const TickAttribBidAsk&) override {
        const int sid = reqId - kBidAskBase;
        if (sid >= 1 && sid <= static_cast<int>(quotes.size())) {
            quotes[static_cast<size_t>(sid - 1)].bid = bidPrice;
            quotes[static_cast<size_t>(sid - 1)].ask = askPrice;
        }
    }

    // Fallback path: conflated streaming updates (live or delayed).
    void tickPrice(TickerId tickerId, TickType field, double price,
                   const TickAttrib&) override {
        const int sid = static_cast<int>(tickerId) - kMktDataBase;
        if (sid < 1 || sid > static_cast<int>(quotes.size())) return;
        const int ft = static_cast<int>(field);
        if (ft == kTickBid || ft == kTickDelayedBid) {
            quotes[static_cast<size_t>(sid - 1)].bid = price;
        } else if (ft == kTickAsk || ft == kTickDelayedAsk) {
            quotes[static_cast<size_t>(sid - 1)].ask = price;
        } else if (ft == kTickLast || ft == kTickDelayedLast) {
            const int64_t now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            emit_tick(static_cast<uint32_t>(sid), now_ns, price, 0.0);
        }
    }
};

// ---- adapter -----------------------------------------------------------------

TwsFeed::TwsFeed(TwsFeedConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {}

TwsFeed::~TwsFeed() { stop(); }

bool TwsFeed::start() {
    if (io_thread_.joinable()) return false;
    stop_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this] { io_loop(); });
    watchdog_thread_ = std::thread([this] { watchdog_loop(); });
    return true;
}

void TwsFeed::stop() {
    stop_.store(true, std::memory_order_release);
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
    // Unblock a possibly-frozen eConnect so the joins return promptly.
    abort_inflight_connect("shutdown");
    if (io_thread_.joinable()) io_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    connected_.store(false, std::memory_order_release);
}

void TwsFeed::abort_inflight_connect(const char* why) {
    std::lock_guard<std::mutex> lk(conn_mu_);
    if (!connecting_) return;
    log(std::string("connect ") + why + " - closing socket to unblock eConnect");
    static_cast<EClientSocket*>(connecting_)->eDisconnect();
    connect_started_ms_.store(0, std::memory_order_release);   // don't re-abort this attempt
    connect_aborts_.fetch_add(1, std::memory_order_relaxed);
}

void TwsFeed::watchdog_loop() {
    // See TwsBroker::watchdog_loop — same wedged-eConnect hazard for the feed.
    constexpr int64_t kConnectTimeoutMs = 30'000;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (stop_.load(std::memory_order_acquire)) break;
        const int64_t started = connect_started_ms_.load(std::memory_order_acquire);
        if (started == 0) continue;                               // no connect in flight
        if (connected_.load(std::memory_order_acquire)) continue; // handshake completed
        if (now_ms() - started < kConnectTimeoutMs) continue;
        abort_inflight_connect("handshake stuck >30s");
    }
}

void TwsFeed::request_reconnect() {
    reconnect_req_.store(true, std::memory_order_release);
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
}

bool TwsFeed::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void TwsFeed::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("tws-feed: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

void TwsFeed::io_loop() {
    Io io(*this);
    wake_.store(&io.signal, std::memory_order_release);

    auto last_connect = std::chrono::steady_clock::time_point{};
    while (!stop_.load(std::memory_order_acquire)) {
        if (!io.client || !io.client->isConnected()) {
            io.drop_connection();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_connect >= std::chrono::seconds(3)) {
                last_connect = now;
                if (!io.connect_gateway())
                    log("cannot reach IB Gateway at " + cfg_.host + ":" +
                        std::to_string(cfg_.port) + " (is it running + API enabled?)");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            io.signal.waitForSignal();
            if (io.reader) io.reader->processMsgs();
            // Half-open guard: socket up but no nextValidId within 10s.
            const bool stalled =
                !connected_.load(std::memory_order_acquire) &&
                std::chrono::steady_clock::now() - last_connect > std::chrono::seconds(10);
            const bool forced = reconnect_req_.exchange(false, std::memory_order_acq_rel);
            if (io.reset_conn || stalled || forced) {
                if (forced) log("scheduled refresh - reconnecting stream");
                else if (stalled && !io.reset_conn)
                    log("no API handshake within 10s - reconnecting");
                io.reset_conn = false;
                io.drop_connection();
                last_connect = std::chrono::steady_clock::now();   // full backoff
            }
        }
    }

    wake_.store(nullptr, std::memory_order_release);
    io.drop_connection();
}

} // namespace tt
