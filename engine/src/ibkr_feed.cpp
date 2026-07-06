#include "engine/ibkr_feed.h"

#include "net_util.h"
#include "net_ws.h"
#include "engine/clock.h"
#include "engine/ibkr_broker.h"   // ibkr_parse_conid, ibkr_parse_first_account

#ifdef _WIN32
#include <windows.h>
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace tt {

using nlohmann::json;
using tt::net_util::num_field;

std::string ibkr_parse_session_token(std::string_view json_text) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {};
    return j.value("session", "");
}

bool ibkr_parse_md_msg(std::string_view json_text, IbkrMdUpdate& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    const std::string topic = j.value("topic", "");
    if (topic == "sts") {
        const auto args = j.find("args");
        if (args != j.end() && args->is_object() &&
            args->value("authenticated", true) == false) {
            out.kind = IbkrMdUpdate::AuthLost;
            return true;
        }
        return false;
    }
    if (topic.rfind("smd+", 0) != 0) return false;
    IbkrMdUpdate u;
    u.kind = IbkrMdUpdate::Market;
    const auto conid = j.find("conid");
    if (conid != j.end() && conid->is_number()) u.conid = conid->get<int64_t>();
    if (u.conid == 0) {
        try {
            u.conid = std::stoll(topic.substr(4));
        } catch (...) {
            return false;
        }
    }
    if (j.contains("31")) {
        u.has_last = true;
        u.last = num_field(j, "31");
    }
    if (j.contains("84")) {
        u.has_bid = true;
        u.bid = num_field(j, "84");
    }
    if (j.contains("86")) {
        u.has_ask = true;
        u.ask = num_field(j, "86");
    }
    if (j.contains("7059")) {
        u.has_size = true;
        u.size = num_field(j, "7059");
    }
    u.ts_ms = static_cast<int64_t>(num_field(j, "_updated"));
    out = u;
    return u.has_last || u.has_bid || u.has_ask;
}

bool ibkr_parse_history_bars(std::string_view json_text, std::vector<RestBar>& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    const auto data = j.find("data");
    if (data == j.end() || !data->is_array()) return false;
    for (const json& b : *data) {
        if (!b.is_object()) continue;
        RestBar r;
        r.ts_ns = static_cast<int64_t>(num_field(b, "t")) * 1'000'000;   // ms
        r.open = num_field(b, "o");
        r.high = num_field(b, "h");
        r.low = num_field(b, "l");
        r.close = num_field(b, "c");
        r.volume = num_field(b, "v");
        if (r.ts_ns > 0 && r.close > 0) out.push_back(r);
    }
    return true;
}

IbkrFeed::IbkrFeed(IbkrFeedConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)) {}

IbkrFeed::~IbkrFeed() { stop(); }

bool IbkrFeed::start() {
    if (io_thread_.joinable()) return false;
    stop_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this] { io_loop(); });
    return true;
}

void IbkrFeed::stop() {
    stop_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

bool IbkrFeed::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void IbkrFeed::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("ibkr-feed: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

void IbkrFeed::io_loop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cfg_.pin_core >= 0 && cfg_.pin_core < 64)
        SetThreadAffinityMask(GetCurrentThread(), 1ull << cfg_.pin_core);
#endif
    net_ensure_curl_init();

    CURL* rest = curl_easy_init();
    auto call = [&](const char* method, const std::string& path, std::string& body_out) {
        body_out.clear();
        if (!rest) return 0L;
        curl_easy_reset(rest);
        const std::string url = cfg_.gateway_url + path;
        curl_easy_setopt(rest, CURLOPT_URL, url.c_str());
        curl_easy_setopt(rest, CURLOPT_SSL_VERIFYPEER, 0L);   // loopback gateway
        curl_easy_setopt(rest, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(rest, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(rest, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(rest, CURLOPT_WRITEFUNCTION, net_util::curl_write_to_string);
        curl_easy_setopt(rest, CURLOPT_WRITEDATA, &body_out);
        curl_easy_setopt(rest, CURLOPT_CUSTOMREQUEST, method);
        long status = 0;
        if (curl_easy_perform(rest) == CURLE_OK)
            curl_easy_getinfo(rest, CURLINFO_RESPONSE_CODE, &status);
        return status;
    };

    WsClient ws;
    std::vector<int64_t> conids(cfg_.symbols.size(), 0);
    struct Cache { double bid = 0, ask = 0, last = 0; };
    std::vector<Cache> cache(cfg_.symbols.size());
    int64_t last_event_ns = 0;
    int64_t last_tic_ms = 0, last_nag_ms = 0;
    std::string body;

    auto resolve = [&]() -> bool {
        if (call("GET", "/iserver/accounts", body) / 100 != 2) {
            const int64_t now = net_steady_ms();
            if (now - last_nag_ms > 30'000) {
                last_nag_ms = now;
                log("no gateway session — log in via browser (default "
                    "https://localhost:5000)");
            }
            return false;
        }
        bool all = true;
        for (size_t i = 0; i < cfg_.symbols.size(); ++i) {
            if (conids[i]) continue;
            if (call("GET", "/iserver/secdef/search?symbol=" + cfg_.symbols[i], body) /
                    100 ==
                2)
                conids[i] = ibkr_parse_conid(body, cfg_.symbols[i]);
            if (!conids[i]) {
                all = false;
                log("cannot resolve conid for " + cfg_.symbols[i]);
            }
        }
        return all;
    };

    auto backfill = [&](int64_t from_ns) {
        const char* bar = cfg_.bar_seconds == 60     ? "1min"
                          : cfg_.bar_seconds == 300  ? "5min"
                          : cfg_.bar_seconds == 900  ? "15min"
                          : cfg_.bar_seconds == 3600 ? "1h"
                                                     : nullptr;
        if (!bar) {
            log("gap backfill skipped: unsupported bar size");
            return;
        }
        for (size_t i = 0; i < cfg_.symbols.size(); ++i) {
            if (!conids[i]) continue;
            if (call("GET",
                     "/iserver/marketdata/history?conid=" + std::to_string(conids[i]) +
                         "&period=6h&bar=" + bar,
                     body) /
                    100 !=
                2)
                continue;
            std::vector<RestBar> bars;
            if (!ibkr_parse_history_bars(body, bars)) continue;
            size_t pushed = 0;
            for (const RestBar& b : bars) {
                if (b.ts_ns <= from_ns) continue;
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::Bar);
                ev.symbol_id = static_cast<uint32_t>(i + 1);
                ev.ts_event_ns = b.ts_ns;
                ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                ev.u.bar.open = b.open;
                ev.u.bar.high = b.high;
                ev.u.bar.low = b.low;
                ev.u.bar.close = b.close;
                ev.u.bar.volume = b.volume;
                if (sink_(ev)) ++pushed;
                else dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            if (pushed)
                log("backfilled " + std::to_string(pushed) + " bars for " +
                    cfg_.symbols[i] + " after stream gap");
        }
    };

    auto connect_ws = [&]() -> bool {
        // The websocket wants the current session token as a cookie.
        if (call("POST", "/tickle", body) / 100 != 2) return false;
        const std::string session = ibkr_parse_session_token(body);
        if (session.empty()) return false;
        if (!ws.connect(cfg_.ws_url, "api=" + session, /*insecure=*/true)) return false;
        // Give the gateway a beat, then subscribe each conid to
        // last/bid/ask/last-size.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (size_t i = 0; i < conids.size(); ++i) {
            if (!conids[i]) continue;
            if (!ws.send_text("smd+" + std::to_string(conids[i]) +
                              R"(+{"fields":["31","84","86","7059"]})"))
                return false;
        }
        return true;
    };

    auto on_msg = [&](std::string_view text) {
        IbkrMdUpdate u;
        if (!ibkr_parse_md_msg(text, u)) return;
        if (u.kind == IbkrMdUpdate::AuthLost) {
            log("gateway session lost — log in again via browser");
            ws.close();
            connected_.store(false, std::memory_order_release);
            return;
        }
        uint32_t sid = 0;
        for (size_t i = 0; i < conids.size(); ++i)
            if (conids[i] == u.conid) {
                sid = static_cast<uint32_t>(i + 1);
                break;
            }
        if (!sid) return;
        Cache& c = cache[sid - 1];
        if (u.has_bid) c.bid = u.bid;
        if (u.has_ask) c.ask = u.ask;
        if (!u.has_last || u.last <= 0.0) return;   // quote-only update: cache it
        c.last = u.last;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::Tick);
        ev.symbol_id = sid;
        ev.ts_event_ns = u.ts_ms > 0 ? u.ts_ms * 1'000'000 : 0;
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.tick.price = u.last;
        ev.u.tick.size = u.has_size ? u.size : 0.0;
        ev.u.tick.bid = c.bid;
        ev.u.tick.ask = c.ask;
        if (ev.ts_event_ns > last_event_ns) last_event_ns = ev.ts_event_ns;
        if (!sink_(ev)) dropped_.fetch_add(1, std::memory_order_relaxed);
    };

    int backoff_s = 1;
    int64_t next_connect_ms = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!ws.open()) {
            if (net_steady_ms() < next_connect_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (resolve() && connect_ws()) {
                connected_.store(true, std::memory_order_release);
                backoff_s = 1;
                log("streaming via gateway (conflated top-of-book)");
                if (last_event_ns > 0) backfill(last_event_ns);
            } else {
                ws.close();
                next_connect_ms = net_steady_ms() + backoff_s * 1000;
                backoff_s = std::min(backoff_s * 2, 30);
            }
            continue;
        }
        // The gateway drops idle websockets: it expects a "tic" heartbeat.
        const int64_t now = net_steady_ms();
        if (now - last_tic_ms > 45'000) {
            last_tic_ms = now;
            ws.send_text("tic");
            call("POST", "/tickle", body);   // keeps the REST session alive too
        }
        const int r = ws.poll(on_msg);
        if (r < 0) {
            connected_.store(false, std::memory_order_release);
            ws.close();
            next_connect_ms = net_steady_ms() + 1000;
            log("stream lost, reconnecting");
        } else if (r == 0) {
            ws.wait_readable(200);
        }
    }
    if (rest) curl_easy_cleanup(rest);
}

} // namespace tt
