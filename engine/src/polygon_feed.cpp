#include "engine/polygon_feed.h"

#include "alpaca_util.h"
#include "alpaca_ws.h"
#include "engine/clock.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace tt {

using nlohmann::json;

// simdjson on-demand, same discipline as the Alpaca feed parser: this runs
// per websocket frame ahead of the ts_ingest_tsc stamp.
size_t polygon_parse_feed_msgs(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               std::vector<AlpacaFeedMsg>& out) {
    namespace od = simdjson::ondemand;
    thread_local od::parser parser;
    thread_local std::string padded;
    padded.assign(json_text);
    padded.reserve(padded.size() + simdjson::SIMDJSON_PADDING);

    size_t appended = 0;
    try {
        od::document doc = parser.iterate(padded.data(), padded.size(), padded.capacity());
        for (od::value elem : doc.get_array()) {
            od::object obj = elem.get_object();
            const std::string_view ev = obj["ev"].get_string();
            AlpacaFeedMsg m;
            if (ev == "T" || ev == "Q") {
                const std::string_view sym = obj["sym"].get_string();
                for (size_t i = 0; i < symbols.size(); ++i)
                    if (symbols[i] == sym) {
                        m.symbol_id = static_cast<uint32_t>(i + 1);
                        break;
                    }
                if (ev == "T") {
                    m.kind = AlpacaFeedMsg::Trade;
                    m.price = double(obj["p"].get_double());
                    double sz = 0;
                    if (obj["s"].get_double().get(sz) != simdjson::SUCCESS)
                        sz = 0;   // odd lots may omit size
                    m.size = sz;
                } else {
                    m.kind = AlpacaFeedMsg::Quote;
                    m.bid = double(obj["bp"].get_double());
                    m.ask = double(obj["ap"].get_double());
                }
                int64_t ts_ms = 0;   // SIP timestamp, Unix ms
                if (obj["t"].get_int64().get(ts_ms) != simdjson::SUCCESS) ts_ms = 0;
                m.ts_ns = ts_ms * 1'000'000;
            } else if (ev == "status") {
                const std::string_view status = obj["status"].get_string();
                if (status == "connected") m.kind = AlpacaFeedMsg::Connected;
                else if (status == "auth_success") m.kind = AlpacaFeedMsg::Authenticated;
                else if (status == "success") m.kind = AlpacaFeedMsg::Subscription;
                else if (status == "auth_failed" || status == "error") {
                    m.kind = AlpacaFeedMsg::Error;
                    std::string_view msg = "unknown error";
                    if (obj["message"].get_string().get(msg) != simdjson::SUCCESS)
                        msg = "unknown error";
                    m.error = std::string(msg);
                } else {
                    continue;
                }
            } else {
                continue;   // aggregates (A/AM), LULDs, ... nothing we route
            }
            out.push_back(std::move(m));
            ++appended;
        }
    } catch (const simdjson::simdjson_error&) {
        // Malformed input: keep whatever parsed cleanly before the error.
    }
    return appended;
}

// Non-hot path (reconnect recovery).
bool polygon_parse_rest_bars(std::string_view json_text, std::vector<AlpacaRestBar>& out) {
    const json j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    const auto results = j.find("results");
    if (results == j.end() || !results->is_array()) return j.value("resultsCount", -1) == 0;
    for (const json& b : *results) {
        if (!b.is_object()) continue;
        AlpacaRestBar r;
        r.ts_ns = static_cast<int64_t>(alpaca::num_field(b, "t")) * 1'000'000;   // ms
        r.open = alpaca::num_field(b, "o");
        r.high = alpaca::num_field(b, "h");
        r.low = alpaca::num_field(b, "l");
        r.close = alpaca::num_field(b, "c");
        r.volume = alpaca::num_field(b, "v");
        if (r.ts_ns > 0 && r.close > 0) out.push_back(r);
    }
    return true;
}

bool polygon_verify_key(const std::string& rest_url, const std::string& api_key,
                        std::string& detail) {
    alpaca_ensure_curl_init();
    CURL* h = curl_easy_init();
    if (!h) {
        detail = "curl init failed";
        return false;
    }
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
    std::string body;
    const std::string url = rest_url + "/v3/reference/tickers?limit=1";
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, alpaca::curl_write_to_string);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        detail = curl_easy_strerror(rc);
        return false;
    }
    if (status == 401 || status == 403) {
        detail = "invalid API key";
        return false;
    }
    if (status / 100 != 2) {
        detail = "HTTP " + std::to_string(status);
        return false;
    }
    detail = "key OK";
    return true;
}

PolygonFeed::PolygonFeed(PolygonFeedConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {}

PolygonFeed::~PolygonFeed() { stop(); }

bool PolygonFeed::start() {
    if (io_thread_.joinable()) return false;
    stop_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this] { io_loop(); });
    return true;
}

void PolygonFeed::stop() {
    stop_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

bool PolygonFeed::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void PolygonFeed::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("polygon: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

void PolygonFeed::io_loop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cfg_.pin_core >= 0 && cfg_.pin_core < 64)
        if (SetThreadAffinityMask(GetCurrentThread(), 1ull << cfg_.pin_core))
            log("feed thread pinned to core " + std::to_string(cfg_.pin_core));
#endif
    alpaca_ensure_curl_init();

    AlpacaWs ws;
    std::vector<AlpacaFeedMsg> msgs;
    struct BidAsk { double bid = 0.0, ask = 0.0; };
    std::vector<BidAsk> quotes(cfg_.symbols.size());
    int64_t last_trade_ns = 0;

    auto backfill = [&](int64_t from_ns) {
        // Polygon aggs support arbitrary minute multiples.
        if (cfg_.bar_seconds % 60 != 0) {
            log("gap backfill skipped: unsupported bar size " +
                std::to_string(cfg_.bar_seconds) + "s");
            return;
        }
        const int mult = cfg_.bar_seconds / 60;
        const int64_t from_ms = from_ns / 1'000'000;
        const int64_t to_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + cfg_.api_key).c_str());
        for (size_t i = 0; i < cfg_.symbols.size(); ++i) {
            CURL* h = curl_easy_init();
            if (!h) break;
            const std::string url = cfg_.rest_url + "/v2/aggs/ticker/" + cfg_.symbols[i] +
                                    "/range/" + std::to_string(mult) + "/minute/" +
                                    std::to_string(from_ms) + "/" + std::to_string(to_ms) +
                                    "?adjusted=true&sort=asc&limit=5000";
            std::string body;
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, alpaca::curl_write_to_string);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
            const CURLcode rc = curl_easy_perform(h);
            long status = 0;
            if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_cleanup(h);
            std::vector<AlpacaRestBar> bars;
            if (rc != CURLE_OK || status / 100 != 2 ||
                !polygon_parse_rest_bars(body, bars)) {
                log("gap backfill failed for " + cfg_.symbols[i]);
                continue;
            }
            size_t pushed = 0;
            for (const AlpacaRestBar& bar : bars) {
                if (bar.ts_ns <= from_ns) continue;
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::Bar);
                ev.symbol_id = static_cast<uint32_t>(i + 1);
                ev.ts_event_ns = bar.ts_ns;
                ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                ev.u.bar.open = bar.open;
                ev.u.bar.high = bar.high;
                ev.u.bar.low = bar.low;
                ev.u.bar.close = bar.close;
                ev.u.bar.volume = bar.volume;
                if (sink_(ev)) ++pushed;
                else dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            if (pushed)
                log("backfilled " + std::to_string(pushed) + " bars for " +
                    cfg_.symbols[i] + " after stream gap");
        }
        curl_slist_free_all(hdrs);
    };

    auto wait_for = [&](AlpacaFeedMsg::Kind kind, std::string& err) {
        return ws.wait(5000, stop_, [&](std::string_view text) {
            msgs.clear();
            polygon_parse_feed_msgs(text, cfg_.symbols, msgs);
            for (const AlpacaFeedMsg& m : msgs) {
                if (m.kind == kind) return true;
                if (m.kind == AlpacaFeedMsg::Error) {
                    err = m.error;
                    return true;
                }
            }
            return false;
        });
    };

    auto handshake = [&]() -> bool {
        std::string err;
        if (!ws.connect(cfg_.stream_url)) {
            log("connect failed");
            return false;
        }
        if (!wait_for(AlpacaFeedMsg::Connected, err) || !err.empty()) {
            log("no welcome from stream" + (err.empty() ? "" : ": " + err));
            return false;
        }
        const json auth{{"action", "auth"}, {"params", cfg_.api_key}};
        if (!ws.send_text(auth.dump()) ||
            !wait_for(AlpacaFeedMsg::Authenticated, err) || !err.empty()) {
            log("auth failed (check POLYGON_API_KEY)" + (err.empty() ? "" : ": " + err));
            return false;
        }
        std::string params;
        for (const std::string& s : cfg_.symbols)
            params += (params.empty() ? "" : ",") + ("T." + s) + ",Q." + s;
        const json sub{{"action", "subscribe"}, {"params", params}};
        if (!ws.send_text(sub.dump()) ||
            !wait_for(AlpacaFeedMsg::Subscription, err) || !err.empty()) {
            log("subscribe failed" + (err.empty() ? "" : ": " + err));
            return false;
        }
        return true;
    };

    auto on_msg = [&](std::string_view text) {
        msgs.clear();
        polygon_parse_feed_msgs(text, cfg_.symbols, msgs);
        for (const AlpacaFeedMsg& m : msgs) {
            if (m.kind == AlpacaFeedMsg::Quote && m.symbol_id) {
                quotes[m.symbol_id - 1] = {m.bid, m.ask};
            } else if (m.kind == AlpacaFeedMsg::Trade && m.symbol_id && m.price > 0.0) {
                if (m.ts_ns > last_trade_ns) last_trade_ns = m.ts_ns;
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::Tick);
                ev.symbol_id = m.symbol_id;
                ev.ts_event_ns = m.ts_ns;
                ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                ev.u.tick.price = m.price;
                ev.u.tick.size = m.size;
                ev.u.tick.bid = quotes[m.symbol_id - 1].bid;
                ev.u.tick.ask = quotes[m.symbol_id - 1].ask;
                if (!sink_(ev)) dropped_.fetch_add(1, std::memory_order_relaxed);
            } else if (m.kind == AlpacaFeedMsg::Error) {
                log("stream error: " + m.error);
            }
        }
    };

    int backoff_s = 1;
    int64_t next_connect_ms = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!ws.open()) {
            if (alpaca_steady_ms() < next_connect_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (handshake()) {
                connected_.store(true, std::memory_order_release);
                backoff_s = 1;
                std::string joined;
                for (const std::string& s : cfg_.symbols)
                    joined += (joined.empty() ? "" : ",") + s;
                log("streaming " + joined);
                if (last_trade_ns > 0) backfill(last_trade_ns);
            } else {
                ws.close();
                next_connect_ms = alpaca_steady_ms() + backoff_s * 1000;
                backoff_s = std::min(backoff_s * 2, 30);
            }
            continue;
        }
        const int r = ws.poll(on_msg);
        if (r < 0) {
            connected_.store(false, std::memory_order_release);
            ws.close();
            next_connect_ms = alpaca_steady_ms() + 1000;
            log("stream lost, reconnecting");
        } else if (r == 0) {
            if (cfg_.busy_poll) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
            } else {
                ws.wait_readable(200);
            }
        }
    }
}

} // namespace tt
