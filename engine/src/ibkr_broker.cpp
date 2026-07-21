#include "engine/ibkr_broker.h"

#include "net_util.h"
#include "net_ws.h"
#include "engine/clock.h"
#include "engine/price_tick.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tt {

namespace {

using nlohmann::json;
using tt::net_util::num_field;

// cOID suffix "tt-<session_ms>-<local_id>" -> local_id, else 0.
uint64_t coid_suffix(const std::string& coid) {
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

std::string str_field(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_number()) return std::to_string(it->get<double>());
    return {};
}

} // namespace

// ---- parsers ---------------------------------------------------------------

bool ibkr_parse_order_response(std::string_view json_text, IbkrOrderResp& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded()) return false;
    // Error shape: {"error":"..."} — surface as message.
    if (j.is_object()) {
        out.message = str_field(j, "error");
        return !out.message.empty();
    }
    if (!j.is_array() || j.empty() || !j[0].is_object()) return false;
    const json& r = j[0];
    out.order_id = str_field(r, "order_id");
    out.reply_id = str_field(r, "id");
    if (r.contains("message")) {
        if (r["message"].is_array() && !r["message"].empty() && r["message"][0].is_string())
            out.message = r["message"][0].get<std::string>();
        else if (r["message"].is_string())
            out.message = r["message"].get<std::string>();
    }
    if (out.message.empty()) out.message = str_field(r, "order_status");
    return !out.order_id.empty() || !out.reply_id.empty();
}

std::string ibkr_parse_first_account(std::string_view json_text) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {};
    const std::string sel = str_field(j, "selectedAccount");
    if (!sel.empty()) return sel;
    const auto it = j.find("accounts");
    if (it != j.end() && it->is_array() && !it->empty() && (*it)[0].is_string())
        return (*it)[0].get<std::string>();
    return {};
}

std::vector<std::string> ibkr_parse_accounts(std::string_view json_text) {
    std::vector<std::string> out;
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    const auto it = j.find("accounts");
    if (it != j.end() && it->is_array())
        for (const json& a : *it)
            if (a.is_string()) out.push_back(a.get<std::string>());
    return out;
}

int64_t ibkr_parse_conid(std::string_view json_text, const std::string& symbol) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return 0;
    int64_t fallback = 0;
    for (const json& r : j) {
        if (!r.is_object()) continue;
        int64_t conid = 0;
        const auto c = r.find("conid");
        if (c != r.end()) {
            if (c->is_number_integer()) conid = c->get<int64_t>();
            else if (c->is_string()) {
                try {
                    conid = std::stoll(c->get<std::string>());
                } catch (...) {}
            }
        }
        if (conid == 0) continue;
        // NOTE r.value() would throw on "symbol": null, which IBKR sends
        // on index/CFD rows — every string field here must use str_field.
        if (str_field(r, "symbol") != symbol) continue;
        if (fallback == 0) fallback = conid;
        // Prefer the row explicitly typed as a US stock. Old gateway builds
        // put secType on the row; current ones nest it under "sections".
        if (str_field(r, "secType") == "STK") return conid;
        const auto secs = r.find("sections");
        if (secs == r.end() || !secs->is_array()) continue;
        for (const json& sec : *secs)
            if (sec.is_object() && str_field(sec, "secType") == "STK") return conid;
    }
    return fallback;
}

bool ibkr_parse_orders(std::string_view json_text, std::vector<IbkrOrderStatus>& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    const auto it = j.find("orders");
    if (it == j.end() || !it->is_array()) return false;
    for (const json& r : *it) {
        if (!r.is_object()) continue;
        IbkrOrderStatus s;
        s.order_id = str_field(r, "orderId");
        s.status = str_field(r, "status");
        if (!s.order_id.empty()) out.push_back(std::move(s));
    }
    return true;
}

bool ibkr_parse_trades(std::string_view json_text, std::vector<IbkrTrade>& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return false;
    for (const json& r : j) {
        if (!r.is_object()) continue;
        IbkrTrade t;
        t.execution_id = str_field(r, "execution_id");
        t.order_ref = str_field(r, "order_ref");
        t.symbol = str_field(r, "symbol");
        t.buy = str_field(r, "side") != "S";
        t.qty = num_field(r, "size");
        t.price = num_field(r, "price");
        t.commission = num_field(r, "commission");
        t.ts_ms = static_cast<int64_t>(num_field(r, "trade_time_r"));
        if (!t.execution_id.empty() && t.qty > 0) out.push_back(std::move(t));
    }
    return true;
}

bool ibkr_parse_positions(std::string_view json_text, std::vector<IbkrPosition>& out) {
    const json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return false;
    for (const json& r : j) {
        if (!r.is_object()) continue;
        IbkrPosition p;
        const auto c = r.find("conid");
        if (c != r.end() && c->is_number()) p.conid = c->get<int64_t>();
        p.qty = num_field(r, "position");
        if (p.conid != 0 && p.qty != 0.0) out.push_back(p);
    }
    return true;
}

// ---- I/O thread -------------------------------------------------------------

struct IbkrBroker::Io {
    IbkrBroker& b;
    CURL* rest = nullptr;
    bool session = false;
    std::string account;
    std::vector<int64_t> conids;             // per symbol_id-1; 0 = unresolved
    std::unordered_map<uint64_t, std::string> ibkr_by_local;
    std::unordered_map<std::string, uint64_t> local_by_ibkr;
    std::unordered_map<uint64_t, std::string> acct_by_local;   // order -> sub-account
    std::unordered_map<uint64_t, uint32_t> protective_sym_;    // stop-loss leg -> symbol

    // Sub-account an order for `symbol_id` routes to (primary if unset).
    std::string acct_for(uint32_t symbol_id) const {
        if (symbol_id >= 1 && symbol_id <= b.cfg_.symbol_accounts.size() &&
            !b.cfg_.symbol_accounts[symbol_id - 1].empty())
            return b.cfg_.symbol_accounts[symbol_id - 1];
        return account;
    }
    std::unordered_map<uint64_t, std::string> last_status;   // local -> status
    std::unordered_set<std::string> seen_execs;
    int64_t last_tickle_ms = 0, last_poll_ms = 0, last_login_nag_ms = 0;

    explicit Io(IbkrBroker& broker) : b(broker) {
        rest = curl_easy_init();
        conids.assign(b.cfg_.symbols.size(), 0);
    }
    ~Io() {
        if (rest) curl_easy_cleanup(rest);
    }

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
        const std::string url = b.cfg_.gateway_url + path;
        curl_easy_setopt(rest, CURLOPT_URL, url.c_str());
        // IBKR's backend 403s any request without a User-Agent.
        curl_easy_setopt(rest, CURLOPT_USERAGENT, "TradeTerminal/1.0");
        // Loopback gateway with a self-signed cert: the hop never leaves the
        // machine, so peer verification is deliberately off.
        curl_easy_setopt(rest, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(rest, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(rest, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(rest, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(rest, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(rest, CURLOPT_WRITEFUNCTION, net_util::curl_write_to_string);
        curl_easy_setopt(rest, CURLOPT_WRITEDATA, &res.body);
        curl_easy_setopt(rest, CURLOPT_CUSTOMREQUEST, method);
        curl_slist* hdrs = nullptr;
        if (!body.empty()) {
            hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
            curl_easy_setopt(rest, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(rest, CURLOPT_POSTFIELDS, body.c_str());
        } else if (std::string_view(method) == "POST") {
            // Empty POST must still carry Content-Length: 0 — the gateway
            // answers 411 without it and the session kick never lands.
            curl_easy_setopt(rest, CURLOPT_POSTFIELDS, "");
        }
        res.rc = curl_easy_perform(rest);
        if (res.rc == CURLE_OK)
            curl_easy_getinfo(rest, CURLINFO_RESPONSE_CODE, &res.status);
        if (hdrs) curl_slist_free_all(hdrs);
        if (res.status == 401) session = false;   // gateway session expired
        return res;
    }

    // Session + account + conid resolution; true once orders can flow.
    bool ensure_ready() {
        if (!session) {
            Response r = call("GET", "/iserver/accounts", "");
            if (!r.ok() && r.status != 0) {
                // Post-login 401 quirk: kick the brokerage session, retry.
                call("POST", "/iserver/auth/status", "");
                call("POST", "/tickle", "");
                r = call("GET", "/iserver/accounts", "");
            }
            if (!r.ok()) {
                const int64_t now = net_steady_ms();
                if (now - last_login_nag_ms > 30'000) {
                    last_login_nag_ms = now;
                    b.log(r.status == 0
                              ? "cannot reach the Client Portal Gateway at " +
                                    b.cfg_.gateway_url
                              : "gateway probe HTTP " + std::to_string(r.status) +
                                    " — log in via Account menu > Sign In > IBKR");
                }
                return false;
            }
            account = ibkr_parse_first_account(r.body);
            if (account.empty()) return false;
            session = true;
            b.log("session OK, account " + account);
        }
        bool all = true;
        for (size_t i = 0; i < b.cfg_.symbols.size(); ++i) {
            if (conids[i] != 0) continue;
            const Response r =
                call("GET", "/iserver/secdef/search?symbol=" + b.cfg_.symbols[i], "");
            if (r.ok()) conids[i] = ibkr_parse_conid(r.body, b.cfg_.symbols[i]);
            if (conids[i] == 0) {
                all = false;
                b.log("cannot resolve conid for " + b.cfg_.symbols[i]);
            }
        }
        return all;
    }

    void tickle() {
        const int64_t now = net_steady_ms();
        if (now - last_tickle_ms < 60'000) return;
        last_tickle_ms = now;
        call("POST", "/tickle", "");
    }

    json order_json(uint64_t local_id, const OrderRequest& r) {
        json o{{"cOID", b.client_prefix_ + std::to_string(local_id)},
               {"conid", conids[r.symbol_id - 1]},
               {"side", r.side == Side::Buy ? "BUY" : "SELL"},
               {"quantity", r.qty},
               {"tif", "DAY"}};
        switch (r.type) {
        case OrdType::Limit:
            o["orderType"] = "LMT";
            o["price"] = snap_to_tick(r.limit_price);
            break;
        case OrdType::Stop:
            o["orderType"] = "STP";
            o["price"] = snap_to_tick(r.stop_price);
            break;
        default:
            o["orderType"] = "MKT";
        }
        return o;
    }

    void handle_submit(const Cmd& c) {
        json orders = json::array();
        orders.push_back(order_json(c.local_id, c.req));
        // A standalone Stop order (not an engine-native bracket leg — e.g. a
        // strategy's own manual OCO exit submitted as its own top-level order)
        // is, functionally, protecting whatever position it exits. Tag it too,
        // so its reject still trips the naked-position safety net. If it's
        // actually an entry stop (no position open yet), run_live's
        // position!=0 check harmlessly no-ops.
        const bool is_bracket_parent = c.req.take_profit > 0.0 || c.req.stop_loss > 0.0;
        if (c.req.type == OrdType::Stop && !is_bracket_parent)
            protective_sym_[c.local_id] = c.req.symbol_id;
        const std::string parent_coid = b.client_prefix_ + std::to_string(c.local_id);
        // Bracket legs: children referencing the parent cOID, own local ids
        // so their fills route back like any other order.
        auto add_leg = [&](OrdType type, double px) {
            OrderRequest leg{};
            leg.symbol_id = c.req.symbol_id;
            leg.side = c.req.side == Side::Buy ? Side::Sell : Side::Buy;
            leg.type = type;
            leg.qty = c.req.qty;
            if (type == OrdType::Limit) leg.limit_price = px;
            else leg.stop_price = px;
            const uint64_t leg_local = b.next_id_.fetch_add(1, std::memory_order_relaxed);
            json lo = order_json(leg_local, leg);
            lo["parentId"] = parent_coid;
            orders.push_back(std::move(lo));
            return leg_local;
        };
        uint64_t tp_local = 0, sl_local = 0;
        if (c.req.take_profit > 0.0) tp_local = add_leg(OrdType::Limit, c.req.take_profit);
        if (c.req.stop_loss > 0.0) {
            sl_local = add_leg(OrdType::Stop, c.req.stop_loss);
            protective_sym_[sl_local] = c.req.symbol_id;   // reject => position naked
        }

        // Route to the symbol's sub-account (primary if unset); remember it so
        // cancels/flatten target the same account.
        const std::string acct = acct_for(c.req.symbol_id);
        acct_by_local[c.local_id] = acct;
        if (tp_local) acct_by_local[tp_local] = acct;
        if (sl_local) acct_by_local[sl_local] = acct;

        const auto t0 = std::chrono::steady_clock::now();
        Response r = call("POST", "/iserver/account/" + acct + "/orders",
                          json{{"orders", orders}}.dump());
        // The gateway may answer with confirmation questions (price caps,
        // size checks); confirm up to 3 rounds — risk checks live in the
        // engine, not in dialog boxes.
        for (int round = 0; round < 3; ++round) {
            IbkrOrderResp resp;
            if (!r.ok() || !ibkr_parse_order_response(r.body, resp)) {
                const std::string reason =
                    r.rc != CURLE_OK ? curl_easy_strerror(r.rc) : r.body.substr(0, 200);
                b.log("order #" + std::to_string(c.local_id) + " rejected: " + reason);
                const auto ps = protective_sym_.find(c.local_id);
                if (ps != protective_sym_.end())
                    b.push_reject(c.local_id, 0, reason, ps->second, true);
                else
                    b.push_reject(c.local_id, 0, reason);
                return;
            }
            if (!resp.order_id.empty()) {
                ibkr_by_local[c.local_id] = resp.order_id;
                local_by_ibkr[resp.order_id] = c.local_id;
                // Real order-path latency: submit -> gateway+IBKR ack (includes
                // any confirmation rounds). This is the number scalp edges must
                // survive.
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                b.ack_lat_.record_ms(ms);   // feeds the fill-sim latency model
                b.log("order #" + std::to_string(c.local_id) + " acked in " +
                      std::to_string(ms) + " ms");
                if (tp_local || sl_local)
                    b.log("order #" + std::to_string(c.local_id) + " bracket legs: #" +
                          std::to_string(tp_local) + (sl_local ? ", #" +
                          std::to_string(sl_local) : ""));
                return;
            }
            b.log("order #" + std::to_string(c.local_id) + " gateway question: " +
                  resp.message + " — confirming");
            r = call("POST", "/iserver/reply/" + resp.reply_id, R"({"confirmed":true})");
        }
        b.log("order #" + std::to_string(c.local_id) + " stuck in confirmation loop");
        {
            const auto ps = protective_sym_.find(c.local_id);
            if (ps != protective_sym_.end())
                b.push_reject(c.local_id, 0, "stuck in gateway confirmation loop",
                             ps->second, true);
            else
                b.push_reject(c.local_id, 0, "stuck in gateway confirmation loop");
        }
    }

    void handle_cmd(const Cmd& c) {
        switch (c.type) {
        case Cmd::Submit:
            handle_submit(c);
            break;
        case Cmd::Cancel: {
            const auto it = ibkr_by_local.find(c.local_id);
            if (it == ibkr_by_local.end()) {
                b.log("cancel #" + std::to_string(c.local_id) + ": no broker id yet");
                return;
            }
            const auto ai = acct_by_local.find(c.local_id);
            const std::string acct = ai != acct_by_local.end() ? ai->second : account;
            call("DELETE", "/iserver/account/" + acct + "/order/" + it->second, "");
            break;
        }
        case Cmd::CancelAll: {
            const Response r = call("GET", "/iserver/account/orders", "");
            std::vector<IbkrOrderStatus> rows;
            if (r.ok() && ibkr_parse_orders(r.body, rows)) {
                int n = 0;
                for (const IbkrOrderStatus& o : rows) {
                    const auto li = local_by_ibkr.find(o.order_id);
                    if (li == local_by_ibkr.end()) continue;
                    if (o.status == "Filled" || o.status == "Cancelled") continue;
                    const auto ai = acct_by_local.find(li->second);
                    const std::string acct = ai != acct_by_local.end() ? ai->second : account;
                    call("DELETE", "/iserver/account/" + acct + "/order/" + o.order_id, "");
                    ++n;
                }
                b.log("cancel-all: " + std::to_string(n) + " working orders");
            }
            break;
        }
        case Cmd::Flatten: {
            // Sweep the primary account plus every distinct sub-account a symbol
            // trades in; closes route back to the right one via handle_submit.
            std::vector<std::string> accts = {account};
            for (const std::string& a : b.cfg_.symbol_accounts)
                if (!a.empty() &&
                    std::find(accts.begin(), accts.end(), a) == accts.end())
                    accts.push_back(a);
            for (const std::string& acct : accts) {
                const Response r = call("GET", "/portfolio/" + acct + "/positions/0", "");
                std::vector<IbkrPosition> pos;
                if (!r.ok() || !ibkr_parse_positions(r.body, pos)) {
                    b.log("flatten: positions fetch failed for " + acct);
                    continue;
                }
                for (const IbkrPosition& p : pos) {
                    for (size_t i = 0; i < conids.size(); ++i) {
                        if (conids[i] != p.conid) continue;
                        Cmd close{};
                        close.type = Cmd::Submit;
                        close.local_id = b.next_id_.fetch_add(1, std::memory_order_relaxed);
                        close.req.symbol_id = static_cast<uint32_t>(i + 1);
                        close.req.side = p.qty > 0 ? Side::Sell : Side::Buy;
                        close.req.type = OrdType::Market;
                        close.req.qty = p.qty > 0 ? p.qty : -p.qty;
                        handle_submit(close);
                    }
                }
            }
            b.log("flatten requested (close all session positions)");
            break;
        }
        }
    }

    void poll() {
        const int64_t now = net_steady_ms();
        if (now - last_poll_ms < 1000) return;
        last_poll_ms = now;

        // Order status: cancels and rejects (fills come from trades below).
        Response r = call("GET", "/iserver/account/orders", "");
        std::vector<IbkrOrderStatus> rows;
        if (r.ok() && ibkr_parse_orders(r.body, rows)) {
            for (const IbkrOrderStatus& o : rows) {
                const auto it = local_by_ibkr.find(o.order_id);
                if (it == local_by_ibkr.end()) continue;
                std::string& prev = last_status[it->second];
                if (prev == o.status) continue;
                prev = o.status;
                if (o.status == "Cancelled" || o.status == "Inactive") {
                    EngineEvent ev{};
                    ev.type = static_cast<uint16_t>(EvType::OrderCancel);
                    ev.flags = o.status == "Inactive" ? kEvFlagRejected : 0;
                    if (o.status == "Inactive") {
                        const auto ps = protective_sym_.find(it->second);
                        if (ps != protective_sym_.end()) {   // naked-position guard rejected
                            ev.flags |= kEvFlagProtective;
                            ev.symbol_id = ps->second;
                        }
                    }
                    ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                    ev.u.order.order_id = it->second;
                    b.push_ev(ev);
                }
            }
        }

        // Executions, deduped by execution id.
        r = call("GET", "/iserver/account/trades", "");
        std::vector<IbkrTrade> trades;
        if (r.ok() && ibkr_parse_trades(r.body, trades)) {
            for (const IbkrTrade& t : trades) {
                if (!seen_execs.insert(t.execution_id).second) continue;
                const uint64_t local = coid_suffix(t.order_ref);
                if (!local) continue;   // pre-session or manual TWS order
                uint32_t sid = 0;
                for (size_t i = 0; i < b.cfg_.symbols.size(); ++i)
                    if (b.cfg_.symbols[i] == t.symbol) {
                        sid = static_cast<uint32_t>(i + 1);
                        break;
                    }
                if (!sid) continue;
                EngineEvent ev{};
                ev.type = static_cast<uint16_t>(EvType::Fill);
                ev.symbol_id = sid;
                ev.ts_event_ns = t.ts_ms * 1'000'000;
                ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
                ev.u.fill.order_id = local;
                ev.u.fill.price = t.price;
                ev.u.fill.qty = t.qty;
                ev.u.fill.fee = t.commission;
                ev.u.fill.side = static_cast<uint8_t>(t.buy ? Side::Buy : Side::Sell);
                b.push_ev(ev);
            }
        }
    }
};

// ---- adapter ----------------------------------------------------------------

IbkrBroker::IbkrBroker(IbkrConfig cfg) : cfg_(std::move(cfg)) {
    using namespace std::chrono;
    client_prefix_ =
        "tt-" +
        std::to_string(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()) +
        "-";
    net_ensure_curl_init();
    net_make_wake_pipe(wake_tx_, wake_rx_);
    io_thread_ = std::thread([this] { io_loop(); });
}

IbkrBroker::~IbkrBroker() {
    stop_.store(true, std::memory_order_release);
    if (wake_tx_ != static_cast<uintptr_t>(-1))
        ::send(static_cast<SOCKET>(wake_tx_), "x", 1, 0);
    if (io_thread_.joinable()) io_thread_.join();
    if (wake_tx_ != static_cast<uintptr_t>(-1)) ::closesocket(static_cast<SOCKET>(wake_tx_));
    if (wake_rx_ != static_cast<uintptr_t>(-1)) ::closesocket(static_cast<SOCKET>(wake_rx_));
}

uint64_t IbkrBroker::submit(const OrderRequest& r, int64_t /*now_ns*/) {
    if (r.symbol_id == 0 || r.symbol_id > cfg_.symbols.size()) return 0;
    if (cfg_.read_only) {
        log("order blocked: account is READ-ONLY (trading disabled)");
        return 0;
    }
    if (!ready()) {
        log("order rejected: gateway session not ready");
        return 0;
    }
    const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    Cmd c;
    c.type = Cmd::Submit;
    c.local_id = id;
    c.req = r;
    return push_cmd(c) ? id : 0;
}

bool IbkrBroker::cancel(uint64_t order_id) {
    Cmd c;
    c.type = Cmd::Cancel;
    c.local_id = order_id;
    return push_cmd(c);
}

void IbkrBroker::cancel_all() {
    Cmd c;
    c.type = Cmd::CancelAll;
    push_cmd(c);
}

void IbkrBroker::flatten() {
    Cmd c;
    c.type = Cmd::Flatten;
    push_cmd(c);
}

bool IbkrBroker::push_cmd(const Cmd& c) {
    if (cmd_ring_->try_push(c)) {
        if (wake_tx_ != static_cast<uintptr_t>(-1))
            ::send(static_cast<SOCKET>(wake_tx_), "x", 1, 0);
        return true;
    }
    log("command dropped: queue full");
    return false;
}

void IbkrBroker::push_ev(const EngineEvent& ev) {
    if (!ev_ring_->try_push(ev)) log("event dropped: ring full");
}

void IbkrBroker::push_reject(uint64_t local_id, int code, std::string msg,
                             uint32_t symbol_id, bool protective) {
    if (code || !msg.empty()) {
        std::lock_guard lock(reject_mu_);
        // Record BEFORE pushing the event so the reason is visible by the time
        // the engine thread drains it. Bounded: rejects are rare, and every
        // entry is normally consumed by take_reject().
        if (reject_reasons_.size() > 512) reject_reasons_.clear();
        reject_reasons_[local_id] = RejectReason{code, std::move(msg)};
    }
    EngineEvent ev{};
    ev.type = static_cast<uint16_t>(EvType::OrderCancel);
    ev.flags = static_cast<uint16_t>(kEvFlagRejected |
                                     (protective ? kEvFlagProtective : 0));
    ev.symbol_id = protective ? symbol_id : 0;
    ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
    ev.u.order.order_id = local_id;
    push_ev(ev);
}

RejectReason IbkrBroker::take_reject(uint64_t order_id) {
    std::lock_guard lock(reject_mu_);
    const auto it = reject_reasons_.find(order_id);
    if (it == reject_reasons_.end()) return {};
    RejectReason r = std::move(it->second);
    reject_reasons_.erase(it);
    return r;
}

void IbkrBroker::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("ibkr: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

bool IbkrBroker::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void IbkrBroker::io_loop() {
    Io io(*this);
    log("connecting to Client Portal Gateway at " + cfg_.gateway_url);

    while (!stop_.load(std::memory_order_relaxed)) {
        const bool was_ready = ready_.load(std::memory_order_relaxed);
        const bool now_ready = io.ensure_ready();
        ready_.store(now_ready, std::memory_order_release);
        if (now_ready && !was_ready) log("ready — orders can flow");

        Cmd c;
        bool worked = false;
        while (cmd_ring_->try_pop(c)) {
            worked = true;
            if (now_ready) io.handle_cmd(c);
            else if (c.type == Cmd::Submit) {
                // Never reached handle_submit, so protective_sym_ was never set for
                // this id — decide protective-ness straight from the request: any
                // standalone Stop (not an engine bracket parent) protects a fill.
                const bool is_bracket = c.req.take_profit > 0.0 || c.req.stop_loss > 0.0;
                const bool prot = c.req.type == OrdType::Stop && !is_bracket;
                push_reject(c.local_id, 0, "gateway not ready", c.req.symbol_id, prot);
            }
        }
        if (now_ready) {
            io.tickle();
            io.poll();
        }
        if (!worked) {
            // Sleep on the wake pipe: a queued command interrupts instantly;
            // the timeout paces the 1 s status/trade polling.
            fd_set rd;
            FD_ZERO(&rd);
            const SOCKET wake = static_cast<SOCKET>(wake_rx_);
            if (wake_rx_ != static_cast<uintptr_t>(-1)) {
                FD_SET(wake, &rd);
                timeval tv{0, now_ready ? 200'000 : 1'000'000};
                ::select(0, &rd, nullptr, nullptr, &tv);
                char buf[64];
                while (::recv(wake, buf, sizeof buf, 0) > 0) {}
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }
}

} // namespace tt
