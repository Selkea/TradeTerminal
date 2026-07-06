#include "net/gateway_data.h"

#include "engine/ibkr_broker.h"   // ibkr_parse_conid, ibkr_parse_first_account
#include "engine/ibkr_feed.h"     // ibkr_parse_history_bars

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace tt::net {

namespace {

using nlohmann::json;

size_t write_cb(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Snapshot values sometimes carry a state prefix ("C213.45" = prior close,
// "H..." = halted); strip anything before the number.
double snap_num(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] < '0' || s[i] > '9') && s[i] != '-' && s[i] != '.') ++i;
    try {
        return std::stod(s.substr(i));
    } catch (...) {
        return 0.0;
    }
}

const char* bar_for(const std::string& interval) {
    if (interval == "1m") return "1min";
    if (interval == "5m") return "5min";
    if (interval == "15m") return "15min";
    if (interval == "1h") return "1h";
    if (interval == "1d") return "1d";
    return nullptr;
}

const char* period_for(const std::string& range) {
    if (range == "1d") return "1d";
    if (range == "5d") return "1w";
    if (range == "1mo") return "1m";
    if (range == "6mo") return "6m";
    if (range == "1y") return "1y";
    if (range == "2y") return "2y";
    if (range == "5y") return "5y";
    if (range == "max") return "15y";
    return nullptr;
}

} // namespace

GatewayData::GatewayData(std::string gateway_url) : gateway_url_(std::move(gateway_url)) {}

GatewayData::~GatewayData() { stop(); }

void GatewayData::start(Callbacks cbs) {
    cbs_ = std::move(cbs);
    curl_global_init(CURL_GLOBAL_DEFAULT);   // refcounted; curl 8.x is threadsafe
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { worker(); });
}

void GatewayData::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

std::string GatewayData::account() const {
    std::lock_guard lock(mu_);
    return account_;
}

std::string GatewayData::login_url() const {
    // https://localhost:5000/v1/api -> https://localhost:5000
    const size_t p = gateway_url_.find("/v1/");
    return p == std::string::npos ? gateway_url_ : gateway_url_.substr(0, p);
}

uint32_t GatewayData::request_candles(const std::string& symbol,
                                      const std::string& interval,
                                      const std::string& range) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mu_);
    reqs_.push_back({id, symbol, interval, range});
    return id;
}

uint32_t GatewayData::subscribe_quotes(const std::vector<std::string>& symbols,
                                       int poll_s) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mu_);
    subs_[id] = symbols;
    poll_s_ = poll_s > 0 ? poll_s : 5;
    return id;
}

void GatewayData::unsubscribe(uint32_t sub_id) {
    std::lock_guard lock(mu_);
    subs_.erase(sub_id);
}

void GatewayData::log(std::string msg) {
    if (cbs_.on_log) cbs_.on_log(std::move(msg));
}

int64_t GatewayData::conid_for(const std::string& symbol) {
    const auto it = conids_.find(symbol);
    if (it != conids_.end()) return it->second;
    CURL* h = static_cast<CURL*>(rest_);
    std::string body;
    curl_easy_reset(h);
    const std::string url = gateway_url_ + "/iserver/secdef/search?symbol=" + symbol;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    // IBKR's backend 403s any request without a User-Agent.
    curl_easy_setopt(h, CURLOPT_USERAGENT, "TradeTerminal/1.0");
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);   // loopback gateway
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    long status = 0;
    if (curl_easy_perform(h) == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status / 100 != 2) return 0;
    const int64_t conid = ibkr_parse_conid(body, symbol);
    if (conid) conids_[symbol] = conid;
    return conid;
}

void GatewayData::worker() {
    rest_ = curl_easy_init();
    CURL* h = static_cast<CURL*>(rest_);

    auto get = [&](const std::string& path, std::string& body) -> long {
        body.clear();
        if (!h) return 0;
        curl_easy_reset(h);
        const std::string url = gateway_url_ + path;
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        // IBKR's backend 403s any request without a User-Agent.
        curl_easy_setopt(h, CURLOPT_USERAGENT, "TradeTerminal/1.0");
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
        long status = 0;
        if (curl_easy_perform(h) == CURLE_OK)
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        return status;
    };
    auto post = [&](const std::string& path) {
        std::string body;
        if (!h) return 0L;
        curl_easy_reset(h);
        const std::string url = gateway_url_ + path;
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_USERAGENT, "TradeTerminal/1.0");
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
        // Empty POST must still carry Content-Length: 0 — the gateway
        // answers 411 without it and the session kick never lands.
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
        long status = 0;
        if (curl_easy_perform(h) == CURLE_OK)
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        return status;
    };

    std::string body;
    int64_t last_session_ms = 0, last_quotes_ms = 0, last_tickle_ms = 0, last_nag_ms = 0;

    while (running_.load(std::memory_order_relaxed)) {
        const int64_t now = steady_ms();

        // Session state drives everything else.
        if (now - last_session_ms > 3000) {
            last_session_ms = now;
            const bool was = connected_.load(std::memory_order_relaxed);
            long status = get("/iserver/accounts", body);
            if (status / 100 != 2 && status != 0) {
                // CP Gateway quirk: after the browser login, /iserver/*
                // endpoints can 401 until something kicks the brokerage
                // session alive. Kick, then retry once.
                post("/iserver/auth/status");
                post("/tickle");
                status = get("/iserver/accounts", body);
            }
            const bool ok = status / 100 == 2;
            if (ok && !was) {
                std::string acct = ibkr_parse_first_account(body);
                {
                    std::lock_guard lock(mu_);
                    account_ = acct;
                }
                connected_.store(true, std::memory_order_release);
                conn_gen_.fetch_add(1, std::memory_order_relaxed);
                log("gateway: session active" + (acct.empty() ? "" : " (" + acct + ")"));
            } else if (!ok) {
                if (was) log("gateway: session lost");
                connected_.store(false, std::memory_order_release);
                {
                    std::lock_guard lock(mu_);
                    account_.clear();
                }
                if (now - last_nag_ms > 30'000) {
                    last_nag_ms = now;
                    if (status == 0)
                        log("gateway: cannot reach " + gateway_url_ +
                            " — is the CLIENT PORTAL gateway running? (IB Gateway/TWS "
                            "is a different product and won't work)");
                    else
                        log("gateway: probe got HTTP " + std::to_string(status) +
                            " — log in via Account menu > Sign In > IBKR");
                }
            }
        }

        if (connected_.load(std::memory_order_relaxed)) {
            if (now - last_tickle_ms > 60'000) {
                last_tickle_ms = now;
                post("/tickle");
            }

            // Candle requests (charts, backtests, sweeps).
            std::vector<CandleReq> reqs;
            {
                std::lock_guard lock(mu_);
                reqs.swap(reqs_);
            }
            for (const CandleReq& r : reqs) {
                const char* bar = bar_for(r.interval);
                const char* period = period_for(r.range);
                const int64_t conid = (bar && period) ? conid_for(r.symbol) : 0;
                if (!conid) {
                    if (cbs_.on_error)
                        cbs_.on_error(r.id, "gateway",
                                      "cannot fetch " + r.symbol + " " + r.interval);
                    continue;
                }
                if (get("/iserver/marketdata/history?conid=" + std::to_string(conid) +
                            "&period=" + period + "&bar=" + bar + "&outsideRth=false",
                        body) /
                        100 !=
                    2) {
                    if (cbs_.on_error)
                        cbs_.on_error(r.id, "gateway", "history fetch failed for " + r.symbol);
                    continue;
                }
                std::vector<RestBar> bars;
                if (!ibkr_parse_history_bars(body, bars)) {
                    if (cbs_.on_error)
                        cbs_.on_error(r.id, "gateway", "bad history payload for " + r.symbol);
                    continue;
                }
                CandleBatch batch;
                batch.id = r.id;
                batch.symbol = r.symbol;
                batch.interval = r.interval;
                batch.candles.reserve(bars.size());
                for (const RestBar& b : bars)
                    batch.candles.push_back(Candle{b.ts_ns / 1'000'000'000, b.open, b.high,
                                                   b.low, b.close, b.volume});
                if (cbs_.on_candles) cbs_.on_candles(std::move(batch));
            }

            // Watchlist quotes via snapshot polling.
            if (now - last_quotes_ms > static_cast<int64_t>(poll_s_) * 1000) {
                last_quotes_ms = now;
                std::vector<std::string> symbols;
                {
                    std::lock_guard lock(mu_);
                    for (const auto& [id, syms] : subs_)
                        for (const std::string& s : syms)
                            if (std::find(symbols.begin(), symbols.end(), s) ==
                                symbols.end())
                                symbols.push_back(s);
                }
                if (!symbols.empty()) {
                    std::string csv;
                    std::vector<std::pair<int64_t, std::string>> order;
                    for (const std::string& s : symbols) {
                        const int64_t c = conid_for(s);
                        if (!c) continue;
                        csv += (csv.empty() ? "" : ",") + std::to_string(c);
                        order.emplace_back(c, s);
                    }
                    if (!csv.empty() &&
                        get("/iserver/marketdata/snapshot?conids=" + csv +
                                "&fields=31,7762",
                            body) /
                                100 ==
                            2) {
                        const json arr = json::parse(body, nullptr, false);
                        if (arr.is_array()) {
                            for (const json& row : arr) {
                                if (!row.is_object()) continue;
                                int64_t conid = 0;
                                const auto c = row.find("conid");
                                if (c != row.end() && c->is_number())
                                    conid = c->get<int64_t>();
                                std::string sym;
                                for (const auto& [cc, ss] : order)
                                    if (cc == conid) sym = ss;
                                if (sym.empty()) continue;
                                Quote q;
                                if (row.contains("31") && row["31"].is_string())
                                    q.price = snap_num(row["31"].get<std::string>());
                                else if (row.contains("31") && row["31"].is_number())
                                    q.price = row["31"].get<double>();
                                if (row.contains("7762") && row["7762"].is_number())
                                    q.day_volume = row["7762"].get<double>();
                                q.ts_ms = static_cast<int64_t>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now()
                                            .time_since_epoch())
                                        .count());
                                if (q.price > 0 && cbs_.on_tick) cbs_.on_tick(sym, q);
                            }
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (h) curl_easy_cleanup(h);
    rest_ = nullptr;
}

} // namespace tt::net
