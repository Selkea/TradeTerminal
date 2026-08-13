#include "engine/tws_broker.h"

#include "engine/clock.h"
#include "engine/price_tick.h"
#include "engine/tws_client_id.h"   // error 326: what it means, in words

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

// Steady-clock milliseconds — the watchdog and connect path share this timebase.
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// Order-error codes that mean "this order is dead" (vs status noise). Anything
// else order-scoped is logged but the order's fate comes via orderStatus.
bool fatal_order_error(int code) {
    switch (code) {
    case 103:   // duplicate order id — the placement never reached the market
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

    // Next TWS order id to spend. Seeded by nextValidId, then only ever moved
    // FORWARD (tws_advance_order_id) — by adoption, by a 103 self-heal, and by a
    // later nextValidId. Mechanism B of 2026-08-13: this was assigned
    // unconditionally from nextValidId and never advanced past the ids of the
    // orders reconciliation had just adopted, so a session could start below the
    // account's high-water mark and every placement collided with "error 103:
    // Duplicate order id" — which pushed no reject, so the engine left those
    // orders Working forever. See engine/tws_client_id.h.
    long next_tws_id = -1;
    // The handshake for the CURRENT connection has completed. Separates
    // "nextValidId arrived" (which can happen again, for a reqIds answer or a
    // reconnect) from "this connection just came up", so the id seed can be
    // refreshed without re-logging "connected" or re-clearing the 326 latch.
    bool handshaked = false;
    // 326 episode bookkeeping, for the tiered retry + delayed page. Steady ms.
    int64_t conflict_since_ms = 0;
    int conflict_attempts = 0;
    bool conflict_paged = false;
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
        // Publish the in-flight socket + start time so the watchdog can abort a
        // handshake that never completes (eConnect below is a blocking, no-timeout
        // read). Cleared under the same lock once eConnect returns, so the
        // watchdog never touches the socket after we start tearing it down.
        {
            std::lock_guard<std::mutex> lk(b.conn_mu_);
            b.connecting_ = client.get();
            b.connect_started_ms_.store(now_ms(), std::memory_order_release);
        }
        const bool ok = client->eConnect(b.cfg_.host.c_str(), b.cfg_.port,
                                          b.cfg_.client_id);
        {
            std::lock_guard<std::mutex> lk(b.conn_mu_);
            b.connecting_ = nullptr;
            b.connect_started_ms_.store(0, std::memory_order_release);
        }
        if (!ok) {   // includes a watchdog-forced abort — retried after backoff
            client.reset();
            return false;
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        // Silent while a client-id conflict is being retried: this line and the
        // "connection closed" below are two thirds of the three-line-every-3s
        // stanza that buried the one sentence that mattered on 2026-08-11. The
        // conflict line said what is happening and the cleared line will say
        // when it ends; nothing in between is worth a word.
        if (!b.client_id_conflict_.load(std::memory_order_acquire))
            b.log("connecting to IB Gateway at " + b.cfg_.host + ":" +
                  std::to_string(b.cfg_.port));
        return true;
    }

    void drop_connection() {
        b.ready_.store(false, std::memory_order_release);
        if (client) client->eDisconnect();
        reader.reset();
        client.reset();
        // next_tws_id is deliberately NOT reset. It is this broker's high-water
        // mark for the account's order ids, and the ids it already spent are
        // still live at the gateway across a reconnect; throwing it away and
        // taking the reconnect's nextValidId is how a session walks backwards
        // into "error 103: Duplicate order id". The `!client` guards below are
        // what stop a submit while disconnected, not this sentinel.
        handshaked = false;
        // An audit that was mid-flight when the socket went has no answer coming.
        // Leaving it "active" would make the auditor wait forever on a request
        // nobody is going to serve — and reporting "no divergence" while blind is
        // the precise failure mode net/book_divergence.h exists to prevent.
        audit_active = false;
        audit_buf_.clear();
    }

    // ---- EWrapper callbacks (I/O thread, inside processMsgs) ----------------
    void nextValidId(OrderId orderId) override {
        // FORWARD ONLY. Two callers reach here and both can carry a value below
        // ids this session has already spent: a mid-session reconnect (the
        // server's seed is per-connection and 2026-08-13 showed it arriving at
        // 59 while the account had reached ~76), and our own reqIds(1) after
        // reconciliation. Assigning it outright — which is what the code did —
        // guarantees the next placement collides. See kTwsDuplicateOrderId.
        next_tws_id = tws_advance_order_id(next_tws_id, static_cast<long>(orderId));
        b.ready_.store(true, std::memory_order_release);
        if (handshaked) return;   // a reqIds answer, not a fresh connection
        handshaked = true;
        // Assume the fresh session has upstream until a 1100 says otherwise (a
        // gateway that came up disconnected sends one within seconds).
        b.upstream_ok_.store(true, std::memory_order_release);
        // The handshake completing IS the proof the id is ours, so it is the one
        // honest place to clear the 326 latch. Without a clear, /diag and
        // /metrics would keep reporting a conflict that ended, and — worse —
        // Watch-IbGateway.ps1 keys its cold relaunch off broker_connected, so a
        // latch that outlived the condition is what turns one gateway restart
        // into one every four minutes for the rest of the day.
        if (b.client_id_conflict_.exchange(false, std::memory_order_acq_rel)) {
            // Only announce the recovery to whoever was told about the outage —
            // the WatchdogTimer rule. The all-clear tag pages Warning, and a
            // Warning for a three-second collision nobody heard about is how a
            // channel gets ignored.
            if (conflict_paged)
                b.log(tws_client_id_cleared_line("the ORDER path", b.cfg_.client_id));
            else
                b.log("API client id " + std::to_string(b.cfg_.client_id) +
                      " was released; the ORDER path is connected.");
        }
        conflict_since_ms = 0;
        conflict_attempts = 0;
        conflict_paged = false;
        b.log("connected (socket API), orders ready");
        // Adopt existing account state on the FIRST connect of the session only;
        // a later reconnect must not re-seed a position the engine now tracks
        // from its own fills.
        if (!recon_ever) start_reconcile();
    }

    void connectionClosed() override {
        b.ready_.store(false, std::memory_order_release);
        if (!b.client_id_conflict_.load(std::memory_order_acquire))
            b.log("connection closed");   // see connect_gateway: no stanza
    }

    void managedAccounts(const std::string& accounts) override {
        b.log("accounts: " + accounts);
    }

    void error(int id, int errorCode, const std::string& errorString,
               const std::string&) override {
        // Gateway<->IBKR connectivity: 1100 = lost, 1101/1102 = restored. These
        // are not order-scoped (id -1); track the upstream link so ready() (the
        // local socket) isn't mistaken for "can trade". See upstream_connected().
        if (errorCode == 1100) {
            b.upstream_ok_.store(false, std::memory_order_release);
            b.log("gateway lost its connection to IBKR (1100) — orders can't reach the market");
        } else if (errorCode == 1101 || errorCode == 1102) {
            b.upstream_ok_.store(true, std::memory_order_release);
        }
        // 21xx = data-farm status noise; 202 = cancel confirmations.
        if (errorCode >= 2100 && errorCode <= 2170) return;
        if (errorCode == 10141) {   // paper disclaimer dialog not clicked yet (IBC lags login)
            b.log("gateway still accepting the paper disclaimer - retrying shortly");
            reset_conn = true;
            return;
        }
        if (errorCode == kTwsClientIdInUse) {
            // Something else already holds this session's orders client id. See
            // engine/tws_client_id.h: the client keeps retrying the SAME id, and
            // since 0.22.0 that id is kTwsOrdersClientId and never moves.
            //
            // Rotating away from a collision is exactly what caused the
            // 2026-08-13 phantom position: IB scopes an order's callbacks to the
            // client that placed it, so a session on a different id adopts
            // resting orders it can neither hear nor cancel. There is no id this
            // app can move to that is safe, so the only options are "keep asking"
            // and "tell a human" — and it does both, on a tier.
            //
            // The episode starts QUIET. Under a fixed id the overwhelmingly
            // common 326 is our own just-reaped session, released within
            // seconds; paging Critical on every lineup swap would teach the
            // operator to swipe away the one that means a foreign program has
            // the order path for the day. The explanation still appears in the
            // log immediately (2026-08-11's defect was silence); only the
            // paging TAG waits for kTwsClientIdPageAfterSec, emitted from
            // io_loop, which is the thing that knows how long it has been.
            if (!b.client_id_conflict_.exchange(true, std::memory_order_acq_rel)) {
                conflict_since_ms = now_ms();
                conflict_attempts = 0;
                conflict_paged = false;
                b.log(tws_client_id_waiting_line("the ORDER path", b.cfg_.client_id,
                                                 kTwsClientIdFastRetrySec));
            }
            return;
        }
        if (errorCode == kTwsDuplicateOrderId) {
            // Mechanism B of 2026-08-13. The placement did NOT reach the market;
            // before 0.22.0 nothing said so — 103 was not in fatal_order_error,
            // so no reject was pushed and the engine left the order Working
            // forever while check_stuck muttered once after 15 s. The operator's
            // manual sell was refused this way for four hours.
            //
            // Self-heal first: advance past the colliding id so the NEXT
            // placement has a chance. `id` is the TWS order id IB refused.
            next_tws_id = tws_advance_order_id(next_tws_id, static_cast<long>(id) + 1);
            b.log(tws_duplicate_order_id_line(static_cast<long>(id), next_tws_id,
                                              b.cfg_.client_id));
            // Then drop the mapping this collision would otherwise corrupt.
            // place() writes local_by_tws[tws_id] = local unconditionally, so a
            // reused id has already overwritten whatever the ADOPTED order at
            // that id was mapped to — leaving it in place would attribute that
            // order's fills to the refused one. Falls through to the reject
            // below, which is what unsticks the engine.
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
            if (errorCode == kTwsDuplicateOrderId) {
                local_by_tws.erase(id);
                tws_by_local.erase(local);
            }
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
        // Ask the server for its next valid id now that adoption has raised our
        // own high-water mark. It cannot hurt — tws_advance_order_id only ever
        // moves forward, so a server answer below the adopted mark is discarded
        // — and it covers the ids this app never saw: orders that were placed
        // and have already filled or been cancelled leave no open order to adopt,
        // but they DID spend ids. Adoption alone cannot know about those.
        //
        // reqIds is answered by nextValidId, which is why `handshaked` exists:
        // without it this answer would re-log "connected" and re-run the
        // reconcile it is being sent from.
        if (client) client->reqIds(1);
        EngineEvent ev{};
        ev.type = static_cast<uint16_t>(EvType::ReconcileEnd);
        ev.ts_ingest_tsc = static_cast<int64_t>(rdtsc());
        b.push_ev(ev);
        b.log("reconcile: complete (next order id " + std::to_string(next_tws_id) + ")");
    }

    // ---- periodic position audit (net/book_divergence.h) --------------------
    //
    // A COMPARE-ONLY re-read of the account's positions, on a timer, during a
    // live session. Deliberately not part of reconciliation and deliberately
    // incapable of seeding anything: it publishes a snapshot the UI thread reads
    // and pushes NO engine event. A drift detector that silently re-seeded the
    // portfolio would repair the number and erase the evidence that the order
    // path had gone deaf — which is the whole failure it exists to catch.
    bool audit_active = false;
    std::vector<TwsBroker::BrokerPosition> audit_buf_;

    void start_audit() {
        if (!client || recon_active || audit_active) return;
        audit_active = true;
        audit_buf_.clear();
        client->reqPositions();
    }

    void position(const std::string& /*account*/, const Contract& contract,
                  Decimal pos, double avgCost) override {
        if (audit_active) {
            const uint32_t sid = sid_for(contract.symbol);
            if (sid == 0) return;   // not a session symbol: not in the app's book
            // Report the SESSION table's spelling, not the contract's, so the
            // two sides of the comparison are string-identical by construction.
            // A case or whitespace difference between the engine's symbol and
            // IB's would otherwise read as "the app holds a position the broker
            // has never heard of" — a Critical page, from a typo.
            const std::string& sym = b.cfg_.symbols[sid - 1];
            const double qty = DecimalFunctions::decimalToDouble(pos);
            // SUM across accounts. reqPositions reports one row per account, and
            // this route does per-symbol sub-account routing (TwsConfig::
            // symbol_accounts), while the engine keeps ONE portfolio spanning
            // them. Keeping the last row instead of the total would report a
            // divergence for every symbol split across two accounts.
            for (auto& e : audit_buf_)
                if (e.symbol == sym) {
                    e.qty += qty;
                    return;
                }
            audit_buf_.push_back({sym, qty});
            return;
        }
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
        if (audit_active) {
            if (client) client->cancelPositions();   // one-shot: stop the stream
            audit_active = false;
            {
                std::lock_guard lk(b.audit_mu_);
                b.audit_positions_ = audit_buf_;
                ++b.audit_seq_;
            }
            audit_buf_.clear();
            return;
        }
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
        // MECHANISM B, fixed at the source. reqAllOpenOrders is the ONLY place
        // this process learns about order ids it did not issue, and every one of
        // them is already spent at the gateway. Not advancing past them is what
        // let a session seeded at 59 collide with everything from 59 to ~76 on
        // 2026-08-13. Do it before anything below can fail out.
        next_tws_id = tws_advance_order_id(next_tws_id, static_cast<long>(orderId) + 1);
        // PART 1's escape hatch. The orders client id is fixed
        // (kTwsOrdersClientId), so an order this app placed always reports that
        // id here — IB puts the placing client on the wire for every open order
        // (EOrderDecoder::decodeClientId). A different id therefore means this
        // order was placed by something else: a build predating 0.22.0 whose
        // orders are still resting, an order typed into the Gateway GUI, another
        // program on the account. We can SEE it and we cannot HEAR it: its
        // fills go to the placing client and cancelOrder answers 10147.
        //
        // It is adopted anyway. Refusing would leave a real position with
        // nothing in the book at all — strictly worse than a book entry that
        // might go stale — so the page IS the mitigation, and book_divergence
        // is the net under it.
        //
        // clientId 0 is NOT exempted. That is the id IB reports for an order
        // typed into the Gateway/TWS window, and such an order is deaf to us in
        // exactly the same way; "a human placed it" is a reason to say so, not a
        // reason to stay quiet. (reqAutoOpenOrders would bind those to us, but
        // only for client id 0, which the order path must not be.)
        if (order.clientId != b.cfg_.client_id)
            b.log(tws_foreign_order_line(contract.symbol, order.orderType,
                                         static_cast<long>(orderId),
                                         order.clientId, b.cfg_.client_id));
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
        // Extended-hours fill (e.g. covering a short after 4pm ET). IBKR only
        // honors this on non-market orders, which is why the manual path forces
        // a limit when it's set.
        parent.outsideRth = cmd.req.outside_rth != 0;
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
    watchdog_thread_ = std::thread([this] { watchdog_loop(); });
}

TwsBroker::~TwsBroker() {
    stop_.store(true, std::memory_order_release);
    // Unblock a frozen eConnect so the join returns promptly. We do NOT poke the
    // reader signal here: it lives on the I/O thread's stack (io.signal), and
    // racing this thread's issueSignal() against the I/O thread destroying it on
    // exit was a latent use-after-free. A connected I/O thread instead wakes from
    // waitForSignal within its 1s timeout, then sees stop_ and exits.
    abort_inflight_connect("shutdown");
    if (io_thread_.joinable()) io_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
}

void TwsBroker::abort_inflight_connect(const char* why, int64_t only_if_started_ms) {
    std::lock_guard<std::mutex> lk(conn_mu_);
    if (!connecting_) return;   // nothing in flight (or already handed off)
    // Re-validate under the lock that the attempt we decided to abort is still
    // the one in flight: a connect can return and a fresh one start between the
    // watchdog's unlocked timeout check and here, and aborting THAT would kill a
    // healthy new handshake. 0 = unconditional (teardown).
    if (only_if_started_ms != 0 &&
        connect_started_ms_.load(std::memory_order_acquire) != only_if_started_ms)
        return;
    log(std::string("connect ") + why + " - closing socket to unblock eConnect");
    static_cast<EClientSocket*>(connecting_)->eDisconnect();
    connect_started_ms_.store(0, std::memory_order_release);   // don't re-abort this attempt
    connecting_ = nullptr;   // and don't touch this socket again — connect_gateway
                             // reset()s it once its eConnect returns
    connect_aborts_.fetch_add(1, std::memory_order_relaxed);
}

void TwsBroker::watchdog_loop() {
    // eConnect()/the initial handshake read blocks with no timeout; if a connect
    // attempt hasn't reached ready_ within this window the gateway is wedged
    // (accepted the socket, never handshaked). Force the socket closed so the
    // blocked eConnect returns and the I/O loop retries after its backoff. 30s
    // is far longer than a healthy handshake (sub-second), so it never fires on
    // a good connect; 250ms poll keeps teardown latency low.
    constexpr int64_t kConnectTimeoutMs = 30'000;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (stop_.load(std::memory_order_acquire)) break;
        const int64_t started = connect_started_ms_.load(std::memory_order_acquire);
        if (started == 0) continue;                            // no connect in flight
        if (ready_.load(std::memory_order_acquire)) continue;  // handshake completed
        if (now_ms() - started < kConnectTimeoutMs) continue;
        abort_inflight_connect("handshake stuck >30s", started);
    }
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

void TwsBroker::request_position_audit() {
    audit_req_.store(true, std::memory_order_release);
    // Wake the I/O thread: it is normally parked in waitForSignal for up to 1s,
    // and an auditor whose cadence quietly drifted a second per round would make
    // its own age figures lie.
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
}

bool TwsBroker::take_position_audit(std::vector<BrokerPosition>& out) {
    std::lock_guard lk(audit_mu_);
    if (audit_seq_ == audit_taken_) return false;   // no new answer
    audit_taken_ = audit_seq_;
    out = audit_positions_;
    return true;
}

void TwsBroker::request_reconnect() {
    reconnect_req_.store(true, std::memory_order_release);
    // Wake the I/O thread so it acts on the flag now rather than at the next wait.
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
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
            // A client-id collision (IB error 326) is refused instantly and
            // cannot be helped by trying harder, so back the retry right off —
            // but NEVER stop. The id we collide with is usually this app's own
            // previous session (App hands out 20-39 in rotation for exactly that
            // reason), and the gateway releases it on its own; a client that
            // gave up would leave a live session with no order path at all,
            // while positions are open, until a human noticed. Io::error logged
            // the one sentence that explains this; connect_gateway and
            // connectionClosed stay silent meanwhile, so the retry costs no log.
            //
            // TIERED since 0.22.0, because the id no longer rotates. The first
            // few retries are fast (kTwsClientIdFastRetrySec): with a fixed id
            // the common 326 is this app's own just-reaped session, and paying
            // 15 s for it on every lineup swap would mean a 15 s order-path
            // outage with positions live at the broker and nothing adopted yet.
            // After that it falls back to the slow cadence, which is what the
            // original 15 s was reasoned about — a conflict lasting all
            // afternoon costing four attempts a minute of nothing.
            const bool conflict = client_id_conflict_.load(std::memory_order_acquire);
            const auto gap =
                conflict ? std::chrono::seconds(tws_client_id_retry_sec(io.conflict_attempts))
                         : std::chrono::seconds(3);
            // The delayed page. The explanation was logged the moment the
            // refusal arrived; the uppercase tag — the one that pages Critical —
            // waits until the conflict has proved it is not our own socket
            // clearing. Emitted here rather than in the callback because this is
            // the loop that knows how long it has been going on.
            if (conflict && !io.conflict_paged && io.conflict_since_ms &&
                now_ms() - io.conflict_since_ms >= kTwsClientIdPageAfterSec * 1000) {
                io.conflict_paged = true;
                log(tws_client_id_conflict_line(
                    "the ORDER path", "nothing this session submits can reach the market",
                    cfg_.host, cfg_.port, cfg_.client_id));
            }
            if (now - last_connect >= gap) {
                last_connect = now;
                if (conflict) ++io.conflict_attempts;
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
            const bool forced = reconnect_req_.exchange(false, std::memory_order_acq_rel);
            if (io.reset_conn || stalled || forced) {
                if (forced) log("scheduled refresh - reconnecting to IB Gateway");
                else if (stalled && !io.reset_conn)
                    log("no API handshake within 10s - reconnecting");
                io.reset_conn = false;
                io.drop_connection();
                last_connect = Clock::now();   // full backoff before the retry
            }
        }

        Cmd c;
        while (cmd_ring_->try_pop(c)) io.handle_cmd(c);
        // Position audit. Consumed unconditionally so a request that arrives
        // while the socket is down is dropped rather than queued — the auditor
        // measures "how long since an ANSWER", and a stale request replayed on
        // reconnect would answer a question about a book minutes in the past.
        if (audit_req_.exchange(false, std::memory_order_acq_rel)) io.start_audit();
        io.flush_stale_execs();
        io.check_stuck();
    }

    wake_.store(nullptr, std::memory_order_release);
    io.drop_connection();
}

} // namespace tt
