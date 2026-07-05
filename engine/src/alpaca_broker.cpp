#include "engine/alpaca_broker.h"

#include "alpaca_util.h"
#include "alpaca_ws.h"
#include "engine/clock.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace tt {

namespace {

using nlohmann::json;
using tt::alpaca::num_field;
using tt::alpaca::rfc3339_ns;

// client_order_id "tt-<session_ms>-<local_id>" -> local_id, else 0.
uint64_t client_id_suffix(const std::string& coid) {
    if (coid.rfind("tt-", 0) != 0) return 0;
    const size_t dash = coid.find_last_of('-');
    if (dash < 3 || dash + 1 >= coid.size()) return 0;
    uint64_t id = 0;
    for (size_t i = dash + 1; i < coid.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(coid[i]))) return 0;
        id = id * 10 + static_cast<uint64_t>(coid[i] - '0');
    }
    return id;
}

// "10", "0.5", "213.45" — Alpaca takes quantities/prices as strings.
std::string fmt_num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s(b);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

constexpr auto write_cb = tt::alpaca::curl_write_to_string;

} // namespace

void alpaca_ensure_curl_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool alpaca_parse_trade_update(std::string_view json_text,
                               const std::vector<std::string>& symbols,
                               AlpacaTradeUpdate& out) {
    const json j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (j.value("stream", "") != "trade_updates") return false;
    const auto data = j.find("data");
    if (data == j.end() || !data->is_object()) return false;

    AlpacaTradeUpdate tu;
    const std::string event = data->value("event", "");
    if (event == "fill" || event == "partial_fill")
        tu.kind = AlpacaTradeUpdate::Fill;
    else if (event == "canceled" || event == "expired")
        tu.kind = AlpacaTradeUpdate::Cancel;
    else if (event == "rejected")
        tu.kind = AlpacaTradeUpdate::Reject;
    else if (event == "new" || event == "accepted" || event == "pending_new")
        tu.kind = AlpacaTradeUpdate::Ack;
    else
        return false;   // replaced/done_for_day/...: nothing to route yet

    const auto order = data->find("order");
    if (order == data->end() || !order->is_object()) return false;
    tu.local_id = client_id_suffix(order->value("client_order_id", ""));
    tu.broker_id = order->value("id", "");
    const std::string sym = order->value("symbol", "");
    for (size_t i = 0; i < symbols.size(); ++i)
        if (symbols[i] == sym) {
            tu.symbol_id = static_cast<uint32_t>(i + 1);
            break;
        }
    tu.side = order->value("side", "") == "sell" ? Side::Sell : Side::Buy;
    if (tu.kind == AlpacaTradeUpdate::Fill) {
        tu.price = num_field(*data, "price");
        if (tu.price <= 0.0) tu.price = num_field(*order, "filled_avg_price");
        tu.qty = num_field(*data, "qty");
        if (tu.qty <= 0.0) tu.qty = num_field(*order, "filled_qty");
        tu.ts_ns = rfc3339_ns(data->value("timestamp", ""));
    }
    out = std::move(tu);
    return true;
}

bool alpaca_verify_account(const std::string& rest_url, const std::string& key_id,
                           const std::string& secret, std::string& detail) {
    alpaca_ensure_curl_init();
    CURL* h = curl_easy_init();
    if (!h) {
        detail = "curl init failed";
        return false;
    }
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("APCA-API-KEY-ID: " + key_id).c_str());
    hdrs = curl_slist_append(hdrs, ("APCA-API-SECRET-KEY: " + secret).c_str());
    std::string body;
    const std::string url = rest_url + "/v2/account";
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
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
        detail = "invalid API key or secret";
        return false;
    }
    if (status / 100 != 2) {
        detail = "HTTP " + std::to_string(status) + ": " + body;
        return false;
    }
    const json j = json::parse(body, nullptr, false);
    detail = "account " + (j.is_object() ? j.value("account_number", "?") : "?") + " " +
             (j.is_object() ? j.value("status", "") : "");
    return true;
}

bool alpaca_parse_positions(std::string_view json_text,
                            const std::vector<std::string>& symbols,
                            std::vector<AlpacaPosition>& out) {
    const json arr = json::parse(json_text, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return false;
    for (const json& j : arr) {
        if (!j.is_object()) continue;
        AlpacaPosition p;
        p.symbol = j.value("symbol", "");
        p.qty = num_field(j, "qty");
        if (j.value("side", "") == "short" && p.qty > 0) p.qty = -p.qty;
        p.avg_price = num_field(j, "avg_entry_price");
        for (size_t i = 0; i < symbols.size(); ++i)
            if (symbols[i] == p.symbol) {
                p.symbol_id = static_cast<uint32_t>(i + 1);
                break;
            }
        if (!p.symbol.empty() && p.qty != 0.0) out.push_back(std::move(p));
    }
    return true;
}

// All libcurl state lives here, on the I/O thread only.
struct AlpacaBroker::Io {
    AlpacaBroker& b;
    CURL* rest = nullptr;
    curl_slist* hdrs = nullptr;
    AlpacaWs ws;
    std::unordered_map<uint64_t, std::string> uuid_by_local;
    // Reverse map: bracket legs get Alpaca-generated client ids, so their
    // trade updates can only be matched by broker uuid.
    std::unordered_map<std::string, uint64_t> local_by_uuid;
    bool reconciled = false;   // positions/cash pulled once per session

    explicit Io(AlpacaBroker& broker) : b(broker) {
        hdrs = curl_slist_append(hdrs, ("APCA-API-KEY-ID: " + b.cfg_.key_id).c_str());
        hdrs = curl_slist_append(hdrs, ("APCA-API-SECRET-KEY: " + b.cfg_.secret).c_str());
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        rest = curl_easy_init();
    }
    ~Io() {
        ws_close();
        if (rest) curl_easy_cleanup(rest);
        curl_slist_free_all(hdrs);
    }

    void ws_close() {
        ws.close();
        b.ready_.store(false, std::memory_order_release);
    }

    // ---- REST ------------------------------------------------------------

    struct Response {
        CURLcode rc = CURLE_FAILED_INIT;
        long status = 0;
        std::string body;
        bool ok() const { return rc == CURLE_OK && status / 100 == 2; }
    };

    Response call(const char* method, const std::string& path, const std::string& body) {
        Response res;
        if (!rest) return res;
        curl_easy_reset(rest);
        const std::string url = b.cfg_.rest_url + path;
        curl_easy_setopt(rest, CURLOPT_URL, url.c_str());
        curl_easy_setopt(rest, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(rest, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
        curl_easy_setopt(rest, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(rest, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(rest, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(rest, CURLOPT_WRITEDATA, &res.body);
        curl_easy_setopt(rest, CURLOPT_CUSTOMREQUEST, method);
        if (!body.empty()) curl_easy_setopt(rest, CURLOPT_POSTFIELDS, body.c_str());
        res.rc = curl_easy_perform(rest);
        if (res.rc == CURLE_OK)
            curl_easy_getinfo(rest, CURLINFO_RESPONSE_CODE, &res.status);
        return res;
    }

    void handle_cmd(const Cmd& c) {
        switch (c.type) {
        case Cmd::Submit: {
            const char* type = c.req.type == OrdType::Market ? "market"
                               : c.req.type == OrdType::Limit ? "limit"
                                                              : "stop";
            json j{{"symbol", b.cfg_.symbols[c.req.symbol_id - 1]},
                   {"qty", fmt_num(c.req.qty)},
                   {"side", c.req.side == Side::Buy ? "buy" : "sell"},
                   {"type", type},
                   {"time_in_force", "day"},
                   {"client_order_id", b.client_prefix_ + std::to_string(c.local_id)}};
            if (c.req.type == OrdType::Limit) j["limit_price"] = fmt_num(c.req.limit_price);
            if (c.req.type == OrdType::Stop) j["stop_price"] = fmt_num(c.req.stop_price);
            if (c.req.take_profit > 0.0 || c.req.stop_loss > 0.0) {
                j["order_class"] = "bracket";
                if (c.req.take_profit > 0.0)
                    j["take_profit"] = {{"limit_price", fmt_num(c.req.take_profit)}};
                if (c.req.stop_loss > 0.0)
                    j["stop_loss"] = {{"stop_price", fmt_num(c.req.stop_loss)}};
            }
            const Response r = call("POST", "/v2/orders", j.dump());
            if (!r.ok()) {
                b.log("order #" + std::to_string(c.local_id) + " rejected: " +
                      (r.rc != CURLE_OK ? curl_easy_strerror(r.rc) : r.body));
                b.push_reject(c.local_id);
                return;
            }
            const json resp = json::parse(r.body, nullptr, false);
            if (resp.is_object() && resp.contains("id")) {
                const std::string uuid = resp.value("id", "");
                uuid_by_local[c.local_id] = uuid;
                local_by_uuid[uuid] = c.local_id;
            }
            // Bracket legs: assign them local ids so their fills route back.
            if (resp.is_object() && resp.contains("legs") && resp["legs"].is_array()) {
                std::string ids;
                for (const json& leg : resp["legs"]) {
                    const std::string uuid = leg.value("id", "");
                    if (uuid.empty()) continue;
                    const uint64_t leg_local =
                        b.next_id_.fetch_add(1, std::memory_order_relaxed);
                    uuid_by_local[leg_local] = uuid;
                    local_by_uuid[uuid] = leg_local;
                    ids += (ids.empty() ? "#" : ", #") + std::to_string(leg_local);
                }
                if (!ids.empty())
                    b.log("order #" + std::to_string(c.local_id) + " bracket legs: " + ids);
            }
            break;
        }
        case Cmd::Cancel: {
            const auto it = uuid_by_local.find(c.local_id);
            if (it == uuid_by_local.end()) {
                b.log("cancel #" + std::to_string(c.local_id) + ": no broker id yet");
                return;
            }
            const Response r = call("DELETE", "/v2/orders/" + it->second, "");
            // 404 = already filled/cancelled; the WS event settles the status.
            if (!r.ok() && r.status != 404)
                b.log("cancel #" + std::to_string(c.local_id) + " failed: " + r.body);
            break;
        }
        case Cmd::CancelAll: {
            const Response r = call("DELETE", "/v2/orders", "");
            b.log(r.rc == CURLE_OK ? "cancel-all requested"
                                   : std::string("cancel-all failed: ") +
                                         curl_easy_strerror(r.rc));
            break;
        }
        case Cmd::Flatten: {
            const Response r = call("DELETE", "/v2/positions?cancel_orders=true", "");
            b.log(r.rc == CURLE_OK ? "flatten requested (close all positions)"
                                   : std::string("flatten failed: ") +
                                         curl_easy_strerror(r.rc));
            break;
        }
        }
    }

    // Session-start reconciliation: adopt the account's real cash and any
    // existing positions in this session's symbols, so a restart mid-day
    // doesn't trade against a phantom flat book.
    void reconcile() {
        const Response acct = call("GET", "/v2/account", "");
        if (acct.ok()) {
            const json j = json::parse(acct.body, nullptr, false);
            if (j.is_object()) {
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::AcctSnap);
                ev.u.acct.cash = num_field(j, "cash");
                b.push_ev(ev);
            }
        } else {
            b.log("reconcile: account fetch failed");
        }
        const Response pos = call("GET", "/v2/positions", "");
        if (!pos.ok()) {
            b.log("reconcile: positions fetch failed");
            return;
        }
        std::vector<AlpacaPosition> rows;
        if (!alpaca_parse_positions(pos.body, b.cfg_.symbols, rows)) return;
        for (const AlpacaPosition& p : rows) {
            if (p.symbol_id == 0) {
                b.log("reconcile: account holds " + p.symbol +
                      " which is NOT part of this session (unmanaged)");
                continue;
            }
            EngineEvent ev{};
            ev.type = static_cast<uint16_t>(EvType::PosSnap);
            ev.symbol_id = p.symbol_id;
            ev.u.pos.qty = p.qty;
            ev.u.pos.avg_price = p.avg_price;
            b.push_ev(ev);
        }
    }

    // ---- websocket ---------------------------------------------------------

    bool ws_connect() {
        if (!ws.connect(b.cfg_.stream_url)) {
            b.log("stream connect failed");
            ws_close();
            return false;
        }
        const json auth{{"action", "auth"}, {"key", b.cfg_.key_id}, {"secret", b.cfg_.secret}};
        bool authorized = false;
        if (!ws.send_text(auth.dump()) ||
            !ws.wait(5000, b.stop_, [&](std::string_view msg) {
                const json j = json::parse(msg, nullptr, false);
                if (j.is_discarded() || j.value("stream", "") != "authorization") return false;
                authorized = j.contains("data") && j["data"].value("status", "") == "authorized";
                return true;   // stop waiting either way
            }) ||
            !authorized) {
            b.log("stream auth failed (check APCA_API_KEY_ID / APCA_API_SECRET_KEY)");
            ws_close();
            return false;
        }
        const json listen{{"action", "listen"}, {"data", {{"streams", {"trade_updates"}}}}};
        if (!ws.send_text(listen.dump()) ||
            !ws.wait(5000, b.stop_, [&](std::string_view msg) {
                const json j = json::parse(msg, nullptr, false);
                return !j.is_discarded() && j.value("stream", "") == "listening";
            })) {
            b.log("stream listen handshake failed");
            ws_close();
            return false;
        }
        return true;
    }

    void on_stream_msg(std::string_view msg) {
        AlpacaTradeUpdate tu;
        if (!alpaca_parse_trade_update(msg, b.cfg_.symbols, tu)) return;
        // Bracket legs carry Alpaca-generated client ids; match by uuid.
        if (tu.local_id == 0 && !tu.broker_id.empty()) {
            const auto it = local_by_uuid.find(tu.broker_id);
            if (it != local_by_uuid.end()) tu.local_id = it->second;
        }
        switch (tu.kind) {
        case AlpacaTradeUpdate::Ack:
            // Redundant with the REST response, but covers a reconnect race.
            if (tu.local_id && !tu.broker_id.empty()) {
                uuid_by_local[tu.local_id] = tu.broker_id;
                local_by_uuid[tu.broker_id] = tu.local_id;
            }
            break;
        case AlpacaTradeUpdate::Fill: {
            if (!tu.local_id || !tu.symbol_id) {
                b.log("ignoring fill for order outside this session");
                return;
            }
            EngineEvent ev{};
            ev.type = static_cast<uint16_t>(EvType::Fill);
            ev.symbol_id = tu.symbol_id;
            ev.ts_event_ns = tu.ts_ns;
            ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
            ev.u.fill.order_id = tu.local_id;
            ev.u.fill.price = tu.price;
            ev.u.fill.qty = tu.qty;
            ev.u.fill.fee = 0.0;   // Alpaca equities are commission-free
            ev.u.fill.side = static_cast<uint8_t>(tu.side);
            b.push_ev(ev);
            break;
        }
        case AlpacaTradeUpdate::Cancel:
        case AlpacaTradeUpdate::Reject: {
            if (!tu.local_id) return;
            EngineEvent ev{};
            ev.type = static_cast<uint16_t>(EvType::OrderCancel);
            ev.flags = tu.kind == AlpacaTradeUpdate::Reject ? kEvFlagRejected : 0;
            ev.symbol_id = tu.symbol_id;
            ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
            ev.u.order.order_id = tu.local_id;
            b.push_ev(ev);
            break;
        }
        case AlpacaTradeUpdate::None:
            break;
        }
    }
};

AlpacaBroker::AlpacaBroker(AlpacaConfig cfg) : cfg_(std::move(cfg)) {
    using namespace std::chrono;
    client_prefix_ =
        "tt-" +
        std::to_string(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()) +
        "-";
    io_thread_ = std::thread([this] { io_loop(); });
}

AlpacaBroker::~AlpacaBroker() {
    stop_.store(true, std::memory_order_release);
    if (io_thread_.joinable()) io_thread_.join();
}

uint64_t AlpacaBroker::submit(const OrderRequest& r, int64_t /*now_ns*/) {
    if (r.symbol_id == 0 || r.symbol_id > cfg_.symbols.size()) return 0;
    if (!ready()) {
        log("order rejected: not connected to Alpaca");
        return 0;
    }
    const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    Cmd c;
    c.type = Cmd::Submit;
    c.local_id = id;
    c.req = r;
    return push_cmd(c) ? id : 0;
}

bool AlpacaBroker::cancel(uint64_t order_id) {
    Cmd c;
    c.type = Cmd::Cancel;
    c.local_id = order_id;
    return push_cmd(c);   // status settles when the WS cancel event arrives
}

void AlpacaBroker::cancel_all() {
    Cmd c;
    c.type = Cmd::CancelAll;
    push_cmd(c);
}

void AlpacaBroker::flatten() {
    Cmd c;
    c.type = Cmd::Flatten;
    push_cmd(c);
}

bool AlpacaBroker::push_cmd(const Cmd& c) {
    if (cmd_ring_->try_push(c)) return true;
    log("command dropped: queue full");
    return false;
}

void AlpacaBroker::push_ev(const EngineEvent& ev) {
    if (!ev_ring_->try_push(ev)) log("event dropped: ring full");
}

void AlpacaBroker::push_reject(uint64_t local_id) {
    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::OrderCancel);
    ev.flags = kEvFlagRejected;
    ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
    ev.u.order.order_id = local_id;
    push_ev(ev);
}

void AlpacaBroker::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("alpaca: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

bool AlpacaBroker::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void AlpacaBroker::io_loop() {
    alpaca_ensure_curl_init();

    Io io(*this);
    const bool paper = cfg_.rest_url.find("paper-api") != std::string::npos;
    log(std::string("connecting (") + (paper ? "paper" : "LIVE") + " endpoint)");

    int backoff_s = 1;
    int64_t next_connect_ms = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        bool worked = false;

        // Commands first: cancel-all/flatten must go out via REST even while
        // the stream is down.
        Cmd c;
        while (cmd_ring_->try_pop(c)) {
            worked = true;
            io.handle_cmd(c);
        }

        if (!io.ws.open() && alpaca_steady_ms() >= next_connect_ms) {
            if (io.ws_connect()) {
                ready_.store(true, std::memory_order_release);
                backoff_s = 1;
                log("connected — trade updates streaming");
                if (!io.reconciled) {
                    io.reconcile();
                    io.reconciled = true;
                }
            } else {
                next_connect_ms = alpaca_steady_ms() + backoff_s * 1000;
                backoff_s = std::min(backoff_s * 2, 30);
            }
        }

        if (io.ws.open()) {
            const int r = io.ws.poll([&](std::string_view msg) { io.on_stream_msg(msg); });
            if (r < 0) {
                log("stream lost, reconnecting");
                io.ws_close();
                next_connect_ms = alpaca_steady_ms() + 1000;
            } else if (r > 0) {
                worked = true;
            }
        }

        if (!worked) {
            // Fills wake us the instant bytes land; the 1 ms cap bounds how
            // long a queued engine command (submit/cancel) can sit — small
            // next to the REST round-trip it precedes.
            if (io.ws.open()) io.ws.wait_readable(1);
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace tt
