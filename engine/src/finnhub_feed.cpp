#include "engine/finnhub_feed.h"

#include "net_util.h"
#include "net_ws.h"
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

// simdjson on-demand, same discipline as the Polygon feed parser: this runs
// per websocket frame ahead of the ts_ingest_tsc stamp. A Finnhub frame is a
// single JSON object: {"type":"trade","data":[...]}, {"type":"error",...},
// or {"type":"ping"} (ignored).
size_t finnhub_parse_feed_msgs(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               std::vector<FeedMsg>& out) {
    namespace od = simdjson::ondemand;
    thread_local od::parser parser;
    thread_local std::string padded;
    padded.assign(json_text);
    padded.reserve(padded.size() + simdjson::SIMDJSON_PADDING);

    size_t appended = 0;
    try {
        od::document doc = parser.iterate(padded.data(), padded.size(), padded.capacity());
        od::object root = doc.get_object();
        std::string_view type;
        if (root["type"].get_string().get(type) != simdjson::SUCCESS) return 0;
        if (type == "trade") {
            for (od::value elem : root["data"].get_array()) {
                od::object tr = elem.get_object();
                std::string_view sym;
                if (tr["s"].get_string().get(sym) != simdjson::SUCCESS) continue;
                FeedMsg m;
                m.kind = FeedMsg::Trade;
                for (size_t i = 0; i < symbols.size(); ++i)
                    if (symbols[i] == sym) {
                        m.symbol_id = static_cast<uint32_t>(i + 1);
                        break;
                    }
                if (!m.symbol_id) continue;   // not in this session's table
                double p = 0;
                if (tr["p"].get_double().get(p) != simdjson::SUCCESS) continue;
                m.price = p;
                double v = 0;
                if (tr["v"].get_double().get(v) != simdjson::SUCCESS) v = 0;   // may omit
                m.size = v;
                int64_t ts_ms = 0;
                if (tr["t"].get_int64().get(ts_ms) != simdjson::SUCCESS) ts_ms = 0;
                m.ts_ns = ts_ms * 1'000'000;
                out.push_back(std::move(m));
                ++appended;
            }
        } else if (type == "error") {
            FeedMsg m;
            m.kind = FeedMsg::Error;
            std::string_view msg = "unknown error";
            if (root["msg"].get_string().get(msg) != simdjson::SUCCESS) msg = "unknown error";
            m.error = std::string(msg);
            out.push_back(std::move(m));
            ++appended;
        }
        // "ping" and anything else: ignore.
    } catch (const simdjson::simdjson_error&) {
        // Malformed input: keep whatever parsed cleanly before the error.
    }
    return appended;
}

bool finnhub_verify_key(const std::string& rest_url, const std::string& api_key,
                        std::string& detail) {
    net_ensure_curl_init();
    CURL* h = curl_easy_init();
    if (!h) {
        detail = "curl init failed";
        return false;
    }
    // Token in a header (not the URL) so the key never lands in request logs.
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("X-Finnhub-Token: " + api_key).c_str());
    std::string body;
    const std::string url = rest_url + "/quote?symbol=AAPL";
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, net_util::curl_write_to_string);
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
    if (status == 429) {
        detail = "rate limited — try again shortly";
        return false;
    }
    if (status / 100 != 2) {
        detail = "HTTP " + std::to_string(status);
        return false;
    }
    detail = "key OK";
    return true;
}

FinnhubFeed::FinnhubFeed(FinnhubFeedConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {}

FinnhubFeed::~FinnhubFeed() { stop(); }

bool FinnhubFeed::start() {
    if (io_thread_.joinable()) return false;
    stop_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this] { io_loop(); });
    return true;
}

void FinnhubFeed::stop() {
    stop_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

bool FinnhubFeed::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void FinnhubFeed::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("finnhub: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

void FinnhubFeed::io_loop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cfg_.pin_core >= 0 && cfg_.pin_core < 64)
        if (SetThreadAffinityMask(GetCurrentThread(), 1ull << cfg_.pin_core))
            log("feed thread pinned to core " + std::to_string(cfg_.pin_core));
#endif
    net_ensure_curl_init();

    WsClient ws;
    std::vector<FeedMsg> msgs;
    int64_t last_trade_ns = 0;

    // Finnhub authenticates via the URL token and sends no welcome or subscribe
    // ack — a bad key surfaces as an {"type":"error"} frame or a closed socket.
    auto handshake = [&]() -> bool {
        const char sep = cfg_.stream_url.find('?') == std::string::npos ? '?' : '&';
        const std::string url = cfg_.stream_url + sep + "token=" + cfg_.api_key;
        if (!ws.connect(url)) {
            log("connect failed");
            return false;
        }
        for (const std::string& s : cfg_.symbols) {
            const json sub{{"type", "subscribe"}, {"symbol", s}};
            if (!ws.send_text(sub.dump())) {
                log("subscribe send failed");
                return false;
            }
        }
        return true;
    };

    auto on_msg = [&](std::string_view text) {
        msgs.clear();
        finnhub_parse_feed_msgs(text, cfg_.symbols, msgs);
        for (const FeedMsg& m : msgs) {
            if (m.kind == FeedMsg::Trade && m.symbol_id && m.price > 0.0) {
                if (m.ts_ns > last_trade_ns) last_trade_ns = m.ts_ns;
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::Tick);
                ev.symbol_id = m.symbol_id;
                ev.ts_event_ns = m.ts_ns;
                ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                ev.u.tick.price = m.price;
                ev.u.tick.size = m.size;
                ev.u.tick.bid = 0.0;   // websocket carries trades only
                ev.u.tick.ask = 0.0;
                if (!sink_(ev)) dropped_.fetch_add(1, std::memory_order_relaxed);
            } else if (m.kind == FeedMsg::Error) {
                log("stream error: " + m.error);
            }
        }
    };

    int backoff_s = 1;
    int64_t next_connect_ms = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!ws.open()) {
            if (net_steady_ms() < next_connect_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (handshake()) {
                connected_.store(true, std::memory_order_release);
                backoff_s = 1;
                std::string joined;
                for (const std::string& s : cfg_.symbols)
                    joined += (joined.empty() ? "" : ",") + s;
                log("streaming " + joined + " (real-time US trades)");
            } else {
                ws.close();
                next_connect_ms = net_steady_ms() + backoff_s * 1000;
                backoff_s = std::min(backoff_s * 2, 30);
            }
            continue;
        }
        const int r = ws.poll(on_msg);
        if (r < 0) {
            connected_.store(false, std::memory_order_release);
            ws.close();
            next_connect_ms = net_steady_ms() + 1000;
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
