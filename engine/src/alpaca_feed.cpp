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

void AlpacaFeed::io_loop() {
#ifdef _WIN32
    // Tick ingest latency = this thread's wakeup + parse time; outrank
    // normal threads so a busy UI never delays market data.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    alpaca_ensure_curl_init();

    AlpacaWs ws;
    std::vector<AlpacaFeedMsg> msgs;
    // Latest quote per symbol, attached to the next trade tick.
    struct BidAsk { double bid = 0.0, ask = 0.0; };
    std::vector<BidAsk> quotes(cfg_.symbols.size());

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
            // Block on the socket: ticks are handled the moment bytes land,
            // not on the next timer expiry. Timeout only bounds stop_ checks.
            ws.wait_readable(200);
        }
    }
}

} // namespace tt
