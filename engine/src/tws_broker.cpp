#include "engine/tws_broker.h"

#include "engine/clock.h"
#include "engine/price_tick.h"

// TWS API (fetched at configure time; see third_party/CMakeLists.txt).
#include "CommissionReport.h"
#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Execution.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace tt {

namespace {
using Clock = std::chrono::steady_clock;

// Order-error codes that mean "this order is dead" (vs status noise). Anything
// else order-scoped is logged but the order's fate comes via orderStatus.
bool fatal_order_error(int code) {
    switch (code) {
    case 110:   // price out of range
    case 200:   // no security definition
    case 201:   // order rejected
    case 203:   // security not allowed for account
    case 321:   // server validation error
        return true;
    default:
        return false;
    }
}
} // namespace

// All TWS API state lives on the I/O thread. DefaultEWrapper stubs the ~100
// callbacks; we override the handful that matter.
struct TwsBroker::Io final : DefaultEWrapper {
    TwsBroker& b;
    EReaderOSSignal signal;
    std::unique_ptr<EClientSocket> client;
    std::unique_ptr<EReader> reader;

    long next_tws_id = -1;   // seeded by nextValidId on connect
    // Handshake rejected (e.g. paper disclaimer not accepted yet): tear down
    // and retry from io_loop — destroying the reader inside a callback is unsafe.
    bool reset_conn = false;
    std::unordered_map<long, uint64_t> local_by_tws;
    std::unordered_map<uint64_t, long> tws_by_local;
    std::unordered_map<uint64_t, Clock::time_point> submit_t;   // ack latency
    std::unordered_set<uint64_t> acked;
    std::unordered_set<uint64_t> done;                // filled/cancelled locals
    std::unordered_map<uint64_t, uint32_t> sid_by_local;
    std::unordered_set<uint64_t> protective;          // stop-loss leg locals
    std::unordered_set<uint64_t> stuck_warned;        // logged-once half-open orders
    Clock::time_point last_stuck_check{};

    // Connect-time reconciliation: replay positions/orders/cash exactly once
    // (recon_ever guards against re-adopting on a mid-session reconnect), ending
    // with a ReconcileEnd event when all three streams have finished.
    static constexpr int kAcctReqId = 9001;   // our reqAccountSummary request id
    bool recon_ever = false;
    bool recon_active = false;
    bool recon_pos_done = false, recon_ord_done = false, recon_acct_done = false;

    // Executions wait (briefly) for their commissionReport so the fee rides
    // the fill event; flushed with fee 0 if the report never shows.
    struct PendingExec {
        EngineEvent ev{};
        Clock::time_point at;
    };
    std::unordered_map<std::string, PendingExec> pending_execs;
    std::unordered_set<std::string> seen_execs;

    std::vector<double> net_pos;   // session position per symbol (from fills)

    explicit Io(TwsBroker& broker) : b(broker), signal(1000) {
        net_pos.assign(b.cfg_.symbols.size(), 0.0);
    }

    // ---- connection ---------------------------------------------------------
    bool connect_gateway() {
        client = std::make_unique<EClientSocket>(this, &signal);
        if (!client->eConnect(b.cfg_.host.c_str(), b.cfg_.port, b.cfg_.client_id)) {
            client.reset();
            return false;
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        b.log("connecting to IB Gateway at " + b.cfg_.host + ":" +
              std::to_string(b.cfg_.port));
        return true;
    }

    void drop_connection() {
        b.ready_.store(false, std::memory_order_release);
        if (client) client->eDisconnect();
        reader.reset();
        client.reset();
        next_tws_id = -1;
    }

    // ---- EWrapper callbacks (I/O thread, inside processMsgs) ----------------
    void nextValidId(OrderId orderId) override {
        next_tws_id = orderId;
        b.ready_.store(true, std::memory_order_release);
        b.log("connected (socket API), orders ready");
        // Adopt existing account state on the FIRST connect of the session only;
        // a later reconnect must not re-seed a position the engine now tracks
        // from its own fills.
        if (!recon_ever) start_reconcile();
    }

    void connectionClosed() override {
        b.ready_.store(false, std::memory_order_release);
        b.log("connection closed");
    }

    void managedAccounts(const std::string& accounts) override {
        b.log("accounts: " + accounts);
    }

    void error(int id, int errorCode, const std::string& errorString,
               const std::string&) override {
        // 21xx = data-farm status noise; 202 = cancel confirmations.
        if (errorCode >= 2100 && errorCode <= 2170) return;
        if (errorCode == 10141) {   // paper disclaimer dialog not clicked yet (IBC lags login)
            b.log("gateway still accepting the paper disclaimer - retrying shortly");
            reset_conn = true;
            return;
        }
        b.log("error " + std::to_string(errorCode) + " (id " + std::to_string(id) +
              "): " + errorString);
        const auto it = local_by_tws.find(id);
        if (it != local_by_tws.end() && fatal_order_error(errorCode) &&
            !done.count(it->second)) {
            const uint64_t local = it->second;
            done.insert(local);
            const bool prot = protective.count(local) != 0;
            const uint32_t sid = sid_by_local.count(local) ? sid_by_local[local] : 0;
            b.push_reject(local, errorCode, errorString, sid, prot);
        }
    }

    void orderStatus(OrderId orderId, const std::string& status, Decimal /*filled*/,
                     Decimal /*remaining*/, double /*avgFillPrice*/, int /*permId*/,
                     int /*parentId*/, double /*lastFillPrice*/, int /*clientId*/,
                     const std::string& /*whyHeld*/, double /*mktCapPrice*/) override {
        const auto it = local_by_tws.find(orderId);
        if (it == local_by_tws.end()) return;
        const uint64_t local = it->second;

        // First status = the ack; this is the order-path latency that matters.
        if (!acked.count(local)) {
            acked.insert(local);
            const auto st = submit_t.find(local);
            if (st != submit_t.end()) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - st->second)
                                    .count();
                b.ack_lat_.record_ms(ms);   // feeds the fill-sim latency model
                b.log("order #" + std::to_string(local) + " acked in " +
                      std::to_string(ms) + " ms (" + status + ")");
            }
        }
        if ((status == "Cancelled" || status == "ApiCancelled" ||
             status == "Inactive") &&
            !done.count(local)) {
            done.insert(local);
            EngineEvent ev{};
            ev.type = static_cast<uint16_t>(EvType::OrderCancel);
            if (status == "Inactive") {
                ev.flags = kEvFlagRejected;
                if (protective.count(local)) {   // naked-position: guard rejected
                    ev.flags |= kEvFlagProtective;
                    ev.symbol_id = sid_by_local.count(local) ? sid_by_local[local] : 0;
                }
            }
            ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
            ev.u.order.order_id = local;
            b.push_ev(ev);
        }
    }

    void execDetails(int /*reqId*/, const Contract& /*contract*/,
                     const Execution& execution) override {
        if (seen_execs.count(execution.execId)) return;
        seen_execs.insert(execution.execId);
        const auto it = local_by_tws.find(static_cast<long>(execution.orderId));
        if (it == local_by_tws.end()) return;
        const uint64_t local = it->second;
        done.insert(local);   // at least partially filled: never auto-reject it

        const uint32_t sid = sid_by_local.count(local) ? sid_by_local[local] : 0;
        const bool buy = execution.side == "BOT";
        const double qty = DecimalFunctions::decimalToDouble(execution.shares);
        if (sid >= 1 && sid <= net_pos.size())
            net_pos[sid - 1] += buy ? qty : -qty;

        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::Fill);
        ev.symbol_id = sid;
        ev.ts_event_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.fill.order_id = local;
        ev.u.fill.price = execution.price;
        ev.u.fill.qty = qty;
        ev.u.fill.fee = 0.0;   // patched by commissionReport below
        ev.u.fill.side = static_cast<uint8_t>(buy ? Side::Buy : Side::Sell);
        pending_execs[execution.execId] = PendingExec{ev, Clock::now()};
    }

    void commissionReport(const CommissionReport& report) override {
        const auto it = pending_execs.find(report.execId);
        if (it == pending_execs.end()) return;
        it->second.ev.u.fill.fee = report.commission;
        b.push_ev(it->second.ev);
        pending_execs.erase(it);
    }

    // ---- connect-time reconciliation ---------------------------------------
    uint32_t sid_for(const std::string& sym) const {
        for (size_t i = 0; i < b.cfg_.symbols.size(); ++i)
            if (b.cfg_.symbols[i] == sym) return static_cast<uint32_t>(i + 1);
        return 0;
    }

    void start_reconcile() {
        if (!client) return;
        recon_ever = true;
        recon_active = true;
        recon_pos_done = recon_ord_done = recon_acct_done = false;
        client->reqPositions();
        client->reqAllOpenOrders();
        client->reqAccountSummary(kAcctReqId, "All", "TotalCashValue");
        b.log("reconcile: requesting positions, open orders, cash");
    }

    void maybe_finish_reconcile() {
        if (!recon_active || !recon_pos_done || !recon_ord_done || !recon_acct_done)
            return;
        recon_active = false;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::ReconcileEnd);
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        b.push_ev(ev);
        b.log("reconcile: complete");
    }

    void position(const std::string& /*account*/, const Contract& contract,
                  Decimal pos, double avgCost) override {
        if (!recon_active) return;
        const uint32_t sid = sid_for(contract.symbol);
        if (sid == 0) return;   // not a session symbol
        const double qty = DecimalFunctions::decimalToDouble(pos);
        if (sid <= net_pos.size()) net_pos[sid - 1] = qty;   // seed session net
        if (qty == 0.0) return;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::PosSnap);
        ev.symbol_id = sid;
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.pos.qty = qty;
        ev.u.pos.avg_price = avgCost;
        b.push_ev(ev);
        b.log("reconcile: position " + contract.symbol + " " +
              std::to_string(qty) + " @ " + std::to_string(avgCost));
    }
    void positionEnd() override {
        if (!recon_active) return;
        if (client) client->cancelPositions();   // one-shot: stop the stream
        recon_pos_done = true;
        maybe_finish_reconcile();
    }

    void openOrder(OrderId orderId, const Contract& contract, const Order& order,
                   const OrderState& /*state*/) override {
        if (!recon_active) return;
        if (local_by_tws.count(orderId)) return;   // already ours this session
        const uint32_t sid = sid_for(contract.symbol);
        if (sid == 0) return;
        const uint64_t local = b.next_id_.fetch_add(1, std::memory_order_relaxed);
        local_by_tws[orderId] = local;
        tws_by_local[local] = orderId;
        sid_by_local[local] = sid;
        acked.insert(local);   // a resting order is already acked
        const OrdType type = order.orderType == "LMT" ? OrdType::Limit
                             : (order.orderType == "STP" ||
                                order.orderType == "STP LMT") ? OrdType::Stop
                                                              : OrdType::Market;
        if (type == OrdType::Stop) protective.insert(local);   // resting stop protects
        const Side side = order.action == "BUY" ? Side::Buy : Side::Sell;
        const double qty = DecimalFunctions::decimalToDouble(order.totalQuantity);
        const double px = type == OrdType::Stop ? order.auxPrice : order.lmtPrice;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::OrderNew);
        ev.symbol_id = sid;
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.order.order_id = local;
        ev.u.order.qty = qty;
        ev.u.order.limit_price = px;
        ev.u.order.side = static_cast<uint8_t>(side);
        ev.u.order.ord_type = static_cast<uint8_t>(type);
        b.push_ev(ev);
        b.log("reconcile: open order " + contract.symbol + " " + order.orderType +
              " " + order.action + " " + std::to_string(qty));
    }
    void openOrderEnd() override {
        if (!recon_active) return;
        recon_ord_done = true;
        maybe_finish_reconcile();
    }

    void accountSummary(int reqId, const std::string& /*account*/,
                        const std::string& tag, const std::string& value,
                        const std::string& /*currency*/) override {
        if (!recon_active || reqId != kAcctReqId || tag != "TotalCashValue") return;
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::AcctSnap);
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        ev.u.acct.cash = std::strtod(value.c_str(), nullptr);
        b.push_ev(ev);
        b.log("reconcile: cash " + value);
    }
    void accountSummaryEnd(int reqId) override {
        if (!recon_active || reqId != kAcctReqId) return;
        if (client) client->cancelAccountSummary(reqId);
        recon_acct_done = true;
        maybe_finish_reconcile();
    }

    // Half-open orders: a submit with no first orderStatus/ack after a while
    // (the ~27s TWS ack outliers). Alert-only — count them for /diag and log
    // each once; the engine/user decides what to do. Throttled to ~1/s.
    void check_stuck() {
        static constexpr int64_t kStuckAckMs = 15'000;
        const auto now = Clock::now();
        if (now - last_stuck_check < std::chrono::seconds(1)) return;
        last_stuck_check = now;
        int cnt = 0;
        for (const auto& [local, t] : submit_t) {
            if (acked.count(local) || done.count(local)) continue;
            const auto age_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
            if (age_ms < kStuckAckMs) continue;
            ++cnt;
            if (stuck_warned.insert(local).second)
                b.log("order #" + std::to_string(local) + " not acked after " +
                      std::to_string(age_ms / 1000) + "s — possible half-open order");
        }
        b.stuck_count_.store(cnt, std::memory_order_relaxed);
    }

    // Executions whose commission report never arrived still become fills.
    void flush_stale_execs() {
        const auto now = Clock::now();
        for (auto it = pending_execs.begin(); it != pending_execs.end();) {
            if (now - it->second.at > std::chrono::seconds(1)) {
                b.push_ev(it->second.ev);
                it = pending_execs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- order plumbing -----------------------------------------------------
    Contract make_contract(uint32_t sid) const {
        Contract c;
        c.symbol = b.cfg_.symbols[sid - 1];
        c.secType = "STK";
        c.exchange = "SMART";
        c.currency = "USD";
        return c;
    }

    std::string account_for(uint32_t sid) const {
        if (sid >= 1 && sid <= b.cfg_.symbol_accounts.size())
            return b.cfg_.symbol_accounts[sid - 1];
        return {};
    }

    long place(uint64_t local, uint32_t sid, const Order& order, const Contract& c) {
        const long tws_id = next_tws_id++;
        local_by_tws[tws_id] = local;
        tws_by_local[local] = tws_id;
        sid_by_local[local] = sid;
        submit_t[local] = Clock::now();
        client->placeOrder(tws_id, c, order);
        return tws_id;
    }

    void handle_submit(const Cmd& cmd) {
        if (next_tws_id < 0 || !client) {
            // place() (which would tag a standalone stop protective) is never
            // reached on this early return — decide protective-ness straight
            // from the request instead: any standalone Stop (not an engine
            // bracket parent) protects a fill.
            const bool is_bracket = cmd.req.take_profit > 0.0 || cmd.req.stop_loss > 0.0;
            const bool prot = cmd.req.type == OrdType::Stop && !is_bracket;
            b.push_reject(cmd.local_id, 0, "not connected to gateway",
                          cmd.req.symbol_id, prot);
            return;
        }
        const uint32_t sid = cmd.req.symbol_id;
        const Contract c = make_contract(sid);
        const bool bracket = cmd.req.take_profit > 0.0 || cmd.req.stop_loss > 0.0;

        Order parent;
        parent.action = cmd.req.side == Side::Buy ? "BUY" : "SELL";
        parent.totalQuantity = DecimalFunctions::doubleToDecimal(cmd.req.qty);
        parent.orderType = cmd.req.type == OrdType::Limit  ? "LMT"
                           : cmd.req.type == OrdType::Stop ? "STP"
                                                           : "MKT";
        if (cmd.req.type == OrdType::Limit) parent.lmtPrice = snap_to_tick(cmd.req.limit_price);
        if (cmd.req.type == OrdType::Stop) parent.auxPrice = snap_to_tick(cmd.req.stop_price);
        parent.tif = "DAY";
        parent.account = account_for(sid);
        parent.transmit = !bracket;   // brackets transmit on the last child
        const long parent_tws = place(cmd.local_id, sid, parent, c);
        // A standalone Stop order (not an engine-native bracket child — e.g. a
        // strategy's own manual OCO exit submitted as its own top-level order)
        // is, functionally, protecting whatever position it exits. Tag it too,
        // so its reject still trips the naked-position safety net below. If
        // it's actually an entry stop (no position open yet), run_live's
        // position!=0 check harmlessly no-ops.
        if (cmd.req.type == OrdType::Stop && !bracket) protective.insert(cmd.local_id);

        if (!bracket) return;
        const std::string exit_action = cmd.req.side == Side::Buy ? "SELL" : "BUY";
        uint64_t tp_local = 0, sl_local = 0;
        if (cmd.req.take_profit > 0.0) {
            Order tp;
            tp.action = exit_action;
            tp.totalQuantity = parent.totalQuantity;
            tp.orderType = "LMT";
            tp.lmtPrice = snap_to_tick(cmd.req.take_profit);
            tp.tif = "DAY";
            tp.account = parent.account;
            tp.parentId = parent_tws;
            tp.transmit = cmd.req.stop_loss <= 0.0;
            tp_local = b.next_id_.fetch_add(1, std::memory_order_relaxed);
            place(tp_local, sid, tp, c);
        }
        if (cmd.req.stop_loss > 0.0) {
            Order sl;
            sl.action = exit_action;
            sl.totalQuantity = parent.totalQuantity;
            sl.orderType = "STP";
            sl.auxPrice = snap_to_tick(cmd.req.stop_loss);
            sl.tif = "DAY";
            sl.account = parent.account;
            sl.parentId = parent_tws;
            sl.transmit = true;   // last child transmits the whole bracket
            sl_local = b.next_id_.fetch_add(1, std::memory_order_relaxed);
            protective.insert(sl_local);   // its reject means the position is naked
            place(sl_local, sid, sl, c);
        }
        if (tp_local || sl_local)
            b.log("order #" + std::to_string(cmd.local_id) + " bracket legs: #" +
                  std::to_string(tp_local) +
                  (sl_local ? ", #" + std::to_string(sl_local) : ""));
    }

    void handle_cmd(const Cmd& cmd) {
        switch (cmd.type) {
        case Cmd::Submit:
            handle_submit(cmd);
            break;
        case Cmd::Cancel: {
            const auto it = tws_by_local.find(cmd.local_id);
            if (it == tws_by_local.end()) {
                b.log("cancel #" + std::to_string(cmd.local_id) + ": unknown order");
                return;
            }
            if (client) client->cancelOrder(it->second, OrderCancel{});
            break;
        }
        case Cmd::CancelAll: {
            int n = 0;
            for (const auto& [local, tws_id] : tws_by_local)
                if (!done.count(local) && client) {
                    client->cancelOrder(tws_id, OrderCancel{});
                    ++n;
                }
            b.log("cancel-all: " + std::to_string(n) + " working orders");
            break;
        }
        case Cmd::Flatten: {
            for (size_t i = 0; i < net_pos.size(); ++i) {
                const double pos = net_pos[i];
                if (pos == 0.0 || !client || next_tws_id < 0) continue;
                Order close;
                close.action = pos > 0 ? "SELL" : "BUY";
                close.totalQuantity = DecimalFunctions::doubleToDecimal(std::abs(pos));
                close.orderType = "MKT";
                close.tif = "DAY";
                close.account = account_for(static_cast<uint32_t>(i + 1));
                close.transmit = true;
                const uint64_t local = b.next_id_.fetch_add(1, std::memory_order_relaxed);
                place(local, static_cast<uint32_t>(i + 1), close,
                      make_contract(static_cast<uint32_t>(i + 1)));
            }
            b.log("flatten requested (close session positions)");
            break;
        }
        }
    }
};

// ---- adapter -----------------------------------------------------------------

TwsBroker::TwsBroker(TwsConfig cfg) : cfg_(std::move(cfg)) {
    io_thread_ = std::thread([this] { io_loop(); });
}

TwsBroker::~TwsBroker() {
    stop_.store(true, std::memory_order_release);
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
    if (io_thread_.joinable()) io_thread_.join();
}

uint64_t TwsBroker::submit(const OrderRequest& r, int64_t /*now_ns*/) {
    if (r.symbol_id == 0 || r.symbol_id > cfg_.symbols.size()) return 0;
    if (cfg_.read_only) {
        log("order blocked: account is READ-ONLY (trading disabled)");
        return 0;
    }
    if (!ready()) {
        log("order rejected: socket API not connected");
        return 0;
    }
    const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    Cmd c;
    c.type = Cmd::Submit;
    c.local_id = id;
    c.req = r;
    return push_cmd(c) ? id : 0;
}

bool TwsBroker::cancel(uint64_t order_id) {
    Cmd c;
    c.type = Cmd::Cancel;
    c.local_id = order_id;
    return push_cmd(c);
}

void TwsBroker::cancel_all() {
    Cmd c;
    c.type = Cmd::CancelAll;
    push_cmd(c);
}

void TwsBroker::flatten() {
    Cmd c;
    c.type = Cmd::Flatten;
    push_cmd(c);
}

bool TwsBroker::push_cmd(const Cmd& c) {
    if (!cmd_ring_->try_push(c)) {
        log("command dropped: queue full");
        return false;
    }
    // Wake the I/O thread immediately (it may be parked in waitForSignal).
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
    return true;
}

void TwsBroker::push_ev(const EngineEvent& ev) {
    if (!ev_ring_->try_push(ev)) log("event dropped: ring full");
}

void TwsBroker::push_reject(uint64_t local_id, int code, std::string msg,
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

RejectReason TwsBroker::take_reject(uint64_t order_id) {
    std::lock_guard lock(reject_mu_);
    const auto it = reject_reasons_.find(order_id);
    if (it == reject_reasons_.end()) return {};
    RejectReason r = std::move(it->second);
    reject_reasons_.erase(it);
    return r;
}

void TwsBroker::log(std::string line) {
    std::lock_guard lock(log_mu_);
    logs_.push_back("tws: " + std::move(line));
    while (logs_.size() > 500) logs_.pop_front();
}

bool TwsBroker::pop_log(std::string& out) {
    std::lock_guard lock(log_mu_);
    if (logs_.empty()) return false;
    out = std::move(logs_.front());
    logs_.pop_front();
    return true;
}

void TwsBroker::io_loop() {
    Io io(*this);
    wake_.store(&io.signal, std::memory_order_release);

    auto last_connect = Clock::time_point{};
    while (!stop_.load(std::memory_order_acquire)) {
        if (!io.client || !io.client->isConnected()) {
            io.drop_connection();
            const auto now = Clock::now();
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
            // Half-open guard: socket up but no nextValidId — the gateway is
            // holding the handshake (rejected client id, mid-login, dialog
            // pending...). Without this the connection wedges silently forever.
            const bool stalled = !ready_.load(std::memory_order_acquire) &&
                                 Clock::now() - last_connect > std::chrono::seconds(10);
            if (io.reset_conn || stalled) {
                if (stalled && !io.reset_conn)
                    log("no API handshake within 10s - reconnecting");
                io.reset_conn = false;
                io.drop_connection();
                last_connect = Clock::now();   // full backoff before the retry
            }
        }

        Cmd c;
        while (cmd_ring_->try_pop(c)) io.handle_cmd(c);
        io.flush_stale_execs();
        io.check_stuck();
    }

    wake_.store(nullptr, std::memory_order_release);
    io.drop_connection();
}

} // namespace tt
