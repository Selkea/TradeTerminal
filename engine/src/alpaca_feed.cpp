#include "engine/alpaca_feed.h"

#include "alpaca_util.h"
#include "alpaca_ws.h"
#include "engine/clock.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

namespace tt {

using nlohmann::json;   // handshake message *building* only — off the hot path

// simdjson on-demand: no DOM, no allocation per message — this runs on the
// feed thread for every websocket frame, ahead of the ts_ingest_tsc stamp.
size_t alpaca_parse_feed_msgs(std::string_view json_text,
                              const std::vector<std::string>& symbols,
                              std::vector<AlpacaFeedMsg>& out) {
    namespace od = simdjson::ondemand;
    // Reused per thread: the parser keeps its scratch buffers, and the padded
    // copy grows once to the high-water mark instead of allocating per call.
    thread_local od::parser parser;
    thread_local std::string padded;
    padded.assign(json_text);
    padded.reserve(padded.size() + simdjson::SIMDJSON_PADDING);

    size_t appended = 0;
    try {
        od::document doc = parser.iterate(padded.data(), padded.size(), padded.capacity());
        for (od::value elem : doc.get_array()) {
            od::object obj = elem.get_object();
            const std::string_view type = obj["T"].get_string();
            AlpacaFeedMsg m;
            if (type == "t" || type == "q") {
                const std::string_view sym = obj["S"].get_string();
                for (size_t i = 0; i < symbols.size(); ++i)
                    if (symbols[i] == sym) {
                        m.symbol_id = static_cast<uint32_t>(i + 1);
                        break;
                    }
                if (type == "t") {
                    m.kind = AlpacaFeedMsg::Trade;
                    m.price = double(obj["p"].get_double());
                    m.size = double(obj["s"].get_double());
                } else {
                    m.kind = AlpacaFeedMsg::Quote;
                    m.bid = double(obj["bp"].get_double());
                    m.ask = double(obj["ap"].get_double());
                }
                std::string_view ts;
                if (!obj["t"].get_string().get(ts)) m.ts_ns = alpaca::rfc3339_ns(ts);
            } else if (type == "success") {
                const std::string_view msg = obj["msg"].get_string();
                if (msg == "connected") m.kind = AlpacaFeedMsg::Connected;
                else if (msg == "authenticated") m.kind = AlpacaFeedMsg::Authenticated;
                else continue;
            } else if (type == "subscription") {
                m.kind = AlpacaFeedMsg::Subscription;
            } else if (type == "error") {
                std::string_view msg = "unknown error";
                obj["msg"].get_string().get(msg);
                int64_t code = 0;
                obj["code"].get_int64().get(code);
                m.kind = AlpacaFeedMsg::Error;
                m.error = std::string(msg) + " (code " + std::to_string(code) + ")";
            } else {
                continue;   // bars, statuses, LULDs, ... nothing we route yet
            }
            out.push_back(std::move(m));
            ++appended;
        }
    } catch (const simdjson::simdjson_error&) {
        // Malformed input: keep whatever parsed cleanly before the error.
    }
    return appended;
}

// Non-hot path (reconnect recovery): nlohmann is fine here.
bool alpaca_parse_rest_bars(std::string_view json_text, std::vector<AlpacaRestBar>& out) {
    const json j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    const auto bars = j.find("bars");
    if (bars == j.end() || !bars->is_array()) return false;
    for (const json& b : *bars) {
        if (!b.is_object()) continue;
        AlpacaRestBar r;
        r.ts_ns = alpaca::rfc3339_ns(b.value("t", ""));
        r.open = alpaca::num_field(b, "o");
        r.high = alpaca::num_field(b, "h");
        r.low = alpaca::num_field(b, "l");
        r.close = alpaca::num_field(b, "c");
        r.volume = alpaca::num_field(b, "v");
        if (r.ts_ns > 0 && r.close > 0) out.push_back(r);
    }
    return true;
}

AlpacaFeed::AlpacaFeed(AlpacaFeedConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {}

AlpacaFeed::~AlpacaFeed() { stop(); }

bool AlpacaFeed::start() {
    if (io_thread_.joinable()) return false;   // already started
    stop_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this] { io_loop(); });
    return true;
}

void AlpacaFeed::stop() {
    stop_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

bool AlpacaFeed::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void AlpacaFeed::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("alpaca-feed: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

namespace {

const char* timeframe_for(int bar_seconds) {
    switch (bar_seconds) {
    case 60: return "1Min";
    case 300: return "5Min";
    case 900: return "15Min";
    case 3600: return "1Hour";
    default: return nullptr;
    }
}

std::string rfc3339_utc(int64_t ns) {
    const std::time_t t = static_cast<std::time_t>(ns / 1'000'000'000);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[24];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

void AlpacaFeed::io_loop() {
#ifdef _WIN32
    // Tick ingest latency = this thread's wakeup + parse time; outrank
    // normal threads so a busy UI never delays market data.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cfg_.pin_core >= 0 && cfg_.pin_core < 64)
        if (SetThreadAffinityMask(GetCurrentThread(), 1ull << cfg_.pin_core))
            log("feed thread pinned to core " + std::to_string(cfg_.pin_core));
#endif
    alpaca_ensure_curl_init();

    AlpacaWs ws;
    std::vector<AlpacaFeedMsg> msgs;
    // Latest quote per symbol, attached to the next trade tick.
    struct BidAsk { double bid = 0.0, ask = 0.0; };
    std::vector<BidAsk> quotes(cfg_.symbols.size());
    int64_t last_trade_ns = 0;   // newest trade seen — backfill anchor

    // After an outage, fetch the bars the stream missed so bar-based
    // strategies resume with continuous indicators instead of a blind gap.
    auto backfill = [&](int64_t from_ns) {
        const char* tf = timeframe_for(cfg_.bar_seconds);
        if (!tf) {
            log("gap backfill skipped: unsupported bar size " +
                std::to_string(cfg_.bar_seconds) + "s");
            return;
        }
        const size_t slash = cfg_.stream_url.find_last_of('/');
        const std::string feed_tier =
            slash == std::string::npos ? "iex" : cfg_.stream_url.substr(slash + 1);
        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, ("APCA-API-KEY-ID: " + cfg_.key_id).c_str());
        hdrs = curl_slist_append(hdrs, ("APCA-API-SECRET-KEY: " + cfg_.secret).c_str());
        for (size_t i = 0; i < cfg_.symbols.size(); ++i) {
            CURL* h = curl_easy_init();
            if (!h) break;
            const std::string url = cfg_.data_rest_url + "/v2/stocks/" +
                                    cfg_.symbols[i] + "/bars?timeframe=" + tf +
                                    "&start=" + rfc3339_utc(from_ns) +
                                    "&limit=1000&adjustment=raw&feed=" + feed_tier;
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
                !alpaca_parse_rest_bars(body, bars)) {
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

    // During the handshake waits, data may already stream; kept minimal —
    // anything non-matching is discarded (a few ticks at connect is fine).
    auto wait_for = [&](AlpacaFeedMsg::Kind kind, std::string& err) {
        return ws.wait(5000, stop_, [&](std::string_view text) {
            msgs.clear();
            alpaca_parse_feed_msgs(text, cfg_.symbols, msgs);
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
            log("no welcome from data stream" + (err.empty() ? "" : ": " + err));
            return false;
        }
        const json auth{{"action", "auth"}, {"key", cfg_.key_id}, {"secret", cfg_.secret}};
        if (!ws.send_text(auth.dump()) ||
            !wait_for(AlpacaFeedMsg::Authenticated, err) || !err.empty()) {
            log("auth failed" + (err.empty() ? "" : ": " + err));
            return false;
        }
        const json sub{{"action", "subscribe"},
                       {"trades", cfg_.symbols},
                       {"quotes", cfg_.symbols}};
        if (!ws.send_text(sub.dump()) ||
            !wait_for(AlpacaFeedMsg::Subscription, err) || !err.empty()) {
            log("subscribe failed" + (err.empty() ? "" : ": " + err));
            return false;
        }
        return true;
    };

    auto on_msg = [&](std::string_view text) {
        msgs.clear();
        alpaca_parse_feed_msgs(text, cfg_.symbols, msgs);
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
                log("streaming " + joined + " (IEX)");
                // Reconnect (not first connect): recover what the outage ate.
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
                // Spin: removes the kernel wakeup from tick ingest entirely.
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
            } else {
                // Block on the socket: ticks are handled the moment bytes
                // land, not on the next timer expiry. Timeout only bounds
                // stop_ checks.
                ws.wait_readable(200);
            }
        }
    }
}

} // namespace tt
