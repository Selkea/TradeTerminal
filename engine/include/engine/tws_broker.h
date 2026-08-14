#pragma once
// IBKR broker adapter via the TWS socket API — the low-latency order route.
// Orders go over a local socket to IB Gateway (or TWS) on this machine, which
// speaks IBKR's binary protocol upstream: order acks in single-digit to tens
// of milliseconds versus ~75 ms through the Client Portal REST gateway.
//
// Same threading contract as IbkrBroker: the engine thread enqueues commands
// (SPSC ring, then pokes the reader signal so the I/O thread wakes
// immediately); one I/O thread owns the EClientSocket and processes all
// callbacks; fills/cancels/rejects come back via poll_event().
//
// Fills are reported per execution (execDetails) and held briefly for the
// matching commissionReport so the fee rides the fill event; flatten closes
// the session's own net position (tracked from our fills), mirroring the
// simulator's "close what this session opened" semantics.

#include "engine/ack_latency.h"
#include "engine/broker.h"
#include "engine/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tt {

struct TwsConfig {
    std::string host = "127.0.0.1";
    int port = 4002;        // IB Gateway: 4002 paper, 4001 live (TWS: 7497/7496)
    int client_id = 7;      // one per connected app, per gateway instance
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
    // Optional per-symbol sub-account (parallel to symbols; "" = default).
    std::vector<std::string> symbol_accounts;
    // When true, submit() refuses every order (see IbkrConfig::read_only).
    bool read_only = false;
};

class TwsBroker final : public IBrokerAdapter {
public:
    explicit TwsBroker(TwsConfig cfg);
    ~TwsBroker() override;   // disconnects and joins the I/O thread

    uint64_t submit(const OrderRequest& r, int64_t now_ns) override;
    bool cancel(uint64_t order_id) override;
    void cancel_all() override;
    void flatten() override;
    bool poll_event(EngineEvent& out) override { return ev_ring_->try_pop(out); }
    RejectReason take_reject(uint64_t order_id) override;
    // submit() is engine-thread-only (see IBrokerAdapter), and the engine asks
    // this immediately after a 0 return, so a plain member is sufficient and no
    // lock is needed. Four distinct refusals used to share one bare 0.
    RejectReason last_submit_reject() const override { return last_submit_reject_; }
    bool ready() const override { return ready_.load(std::memory_order_acquire); }
    // The LOCAL app<->gateway API socket being up (ready()) does NOT mean the
    // gateway itself is connected to IBKR: during an IBKR maintenance window /
    // weekend reset the gateway drops its upstream ("error 1100") while still
    // accepting our socket. This tracks that upstream link (1100 lost, 1102/1101
    // restored) so /diag and the watchdog reflect whether orders can ACTUALLY
    // reach IBKR, not just that the socket is open. True until a 1100 says else.
    bool upstream_connected() const { return upstream_ok_.load(std::memory_order_acquire); }
    // Something already holds this adapter's TWS API client id (IB error 326).
    // Latched for the length of the EPISODE, not the process: the I/O loop is
    // still reconnecting, every kTwsClientIdRetrySec, and a completed handshake
    // clears this. So it means "not connected, and here is the reason", never
    // "gave up" — distinct from "gateway unreachable", where the gateway is the
    // thing at fault rather than us being the second comer.
    // See engine/tws_client_id.h.
    bool client_id_conflict() const {
        return client_id_conflict_.load(std::memory_order_acquire);
    }
    // Replays existing positions, resting orders, and cash once on connect so a
    // restarted session adopts them (reqPositions/reqAllOpenOrders/account cash
    // -> PosSnap/OrderNew/AcctSnap, then ReconcileEnd). See the Io reconcile path.
    bool reconciles() const override { return true; }

    // Status/log lines (I/O thread produces, UI drains each frame).
    bool pop_log(std::string& out);

    // Measured order-path latency (submit -> first ack), for the fill sim.
    AckSummary ack_latency() const { return ack_lat_.summary(); }

    // Orders submitted but not acked within the stuck threshold (half-open):
    // surfaced in /diag as an alert; the adapter takes no automatic action.
    int stuck_order_count() const { return stuck_count_.load(std::memory_order_relaxed); }

    // Times the connect-timeout watchdog force-aborted a stuck handshake
    // (see watchdog_loop). Nonzero means eConnect wedged and self-healed —
    // surfaced in /diag so a wedging gateway is visible.
    int connect_aborts() const { return connect_aborts_.load(std::memory_order_relaxed); }

    // Force a one-shot drop + reconnect on the I/O thread (the existing reconnect
    // loop re-establishes + re-handshakes). Used for the scheduled daily refresh
    // that clears IBKR's overnight-reset staleness. Positions are NOT re-adopted
    // (adoption is first-connect only), so an open session keeps its state.
    void request_reconnect();

    // ---- periodic position audit (compare-only) -----------------------------
    //
    // Re-reads the ACCOUNT's positions mid-session so the app can check its own
    // book against the only authority there is. It exists because on 2026-08-13
    // the app carried a phantom 411-share position for 4 h 03 m with every other
    // signal reading healthy: the exit had filled at the broker and the fill
    // callback went to a client id that no longer existed, and there is no
    // callback for a callback that never arrives. See net/book_divergence.h.
    //
    // Strictly separate from connect-time reconciliation. This path pushes NO
    // engine event and seeds NOTHING — it publishes a snapshot for the UI thread
    // to compare. Re-seeding on divergence would fix the number and hide the
    // deafness underneath it.
    struct BrokerPosition {
        std::string symbol;   // the SESSION table's spelling (see Io::position)
        double qty = 0.0;     // signed; negative = short
    };
    // UI thread: ask for a fresh snapshot. Cheap and account-level (reqPositions
    // is not under the historical-data pacing rules), and a no-op while
    // reconciliation or a previous audit is still in flight.
    void request_position_audit();
    // UI thread: take the newest COMPLETED snapshot. False until a new one has
    // landed since the last take — so a caller that gets false learns "no answer
    // yet", which is exactly the state the auditor must be able to see.
    bool take_position_audit(std::vector<BrokerPosition>& out);

private:
    struct Cmd {
        enum : uint8_t { Submit = 1, Cancel, CancelAll, Flatten } type = Submit;
        uint64_t local_id = 0;
        OrderRequest req{};
    };
    struct Io;   // defined in tws_broker.cpp; owns all TWS API state

    void io_loop();
    void watchdog_loop();
    // Force-abort an in-flight eConnect that hasn't handshaked yet by closing
    // its socket from off the I/O thread (unblocks the blocking eConnect read).
    // Used by the watchdog on timeout and by teardown to unblock a frozen
    // connect. why = short reason for the log line. No-op if nothing is in flight.
    // only_if_started_ms != 0 aborts ONLY if that exact attempt is still in
    // flight (re-checked under conn_mu_), so the watchdog can't tear down a fresh
    // connect that started after its unlocked timeout check; 0 = unconditional.
    void abort_inflight_connect(const char* why, int64_t only_if_started_ms = 0);
    void push_ev(const EngineEvent& ev);
    // Record a reject reason (I/O thread) then push the Rejected event. The
    // reason is ALWAYS stored now, even when IB gave no numeric code: a blank
    // entry and a missing entry were indistinguishable to the engine, and the
    // missing one is what /diag rendered as reject_code 0 / reject_msg "" on
    // 2026-08-13. symbol_id + protective mark a rejected protective stop leg so
    // the engine can flatten the position it was guarding (kEvFlagProtective).
    void push_reject(uint64_t local_id, RejectCause cause, int code = 0,
                     std::string msg = {}, uint32_t symbol_id = 0,
                     bool protective = false);
    void log(std::string line);
    bool push_cmd(const Cmd& c);

    TwsConfig cfg_;

    using CmdRing = SpscRing<Cmd, 1 << 10>;
    using EvRing = SpscRing<EngineEvent, 1 << 12>;
    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::unique_ptr<EvRing> ev_ring_ = std::make_unique<EvRing>();

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> ready_{false};
    std::atomic<bool> upstream_ok_{true};   // gateway<->IBKR link (error 1100/1102)
    // IB error 326. Set by the I/O thread on the refusal and cleared by it on
    // the next completed handshake; read by the I/O loop (which slows its retry)
    // and the UI thread (/diag, /metrics). An order path that comes back
    // mid-session does NOT re-run reconciliation — Io::nextValidId gates that on
    // recon_ever, i.e. the first connect of the session only — so recovering is
    // strictly better than staying down, which is why this is not one-way.
    std::atomic<bool> client_id_conflict_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnect_req_{false};   // scheduled daily refresh: drop + reconnect
    // Position audit. The UI thread sets audit_req_; the I/O thread issues the
    // reqPositions and, on positionEnd, publishes under audit_mu_ and bumps
    // audit_seq_. take_position_audit compares audit_seq_ against what it last
    // handed out, so "nothing new since last time" is distinguishable from "the
    // books agree" — the distinction the whole detector rests on.
    std::atomic<bool> audit_req_{false};
    std::mutex audit_mu_;
    std::vector<BrokerPosition> audit_positions_;
    uint64_t audit_seq_ = 0;      // guarded by audit_mu_
    uint64_t audit_taken_ = 0;    // guarded by audit_mu_ (UI thread only reader)
    // The I/O thread's reader signal while it exists (EReaderOSSignal*);
    // push_cmd pokes it so a submitted order is picked up immediately instead
    // of on the next wait timeout.
    std::atomic<void*> wake_{nullptr};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    // Reject reasons keyed by local order id: I/O thread writes on reject, the
    // engine thread consumes via take_reject(). Small and short-lived — an entry
    // lives only until the matching Rejected event is drained.
    std::mutex reject_mu_;
    std::unordered_map<uint64_t, RejectReason> reject_reasons_;
    // Engine thread only: why the last submit() returned 0.
    RejectReason last_submit_reject_{};

    AckLatency ack_lat_;   // recorded on the I/O thread, read from the UI thread
    std::atomic<int> stuck_count_{0};   // I/O thread writes, UI thread reads

    // Connect-timeout watchdog. EClientSocket::eConnect() does a synchronous,
    // no-timeout handshake read after the TCP connect; a gateway that accepts
    // the socket but never completes the API handshake (seen during IBKR's
    // overnight maintenance) freezes the I/O thread indefinitely — the io_loop's
    // 10s stall guard can't run because the thread is stuck *inside* eConnect.
    // A separate watchdog thread aborts a connect stuck past the timeout by
    // closing its socket, which unblocks eConnect so the I/O loop retries.
    // conn_mu_ guards connecting_ so the watchdog never eDisconnect()s a socket
    // the I/O thread is concurrently destroying.
    std::mutex conn_mu_;
    void* connecting_ = nullptr;                  // EClientSocket* in flight (guarded by conn_mu_)
    std::atomic<int64_t> connect_started_ms_{0};  // steady-clock ms when eConnect began; 0 = idle
    std::atomic<int> connect_aborts_{0};          // watchdog force-aborts (I/O + watchdog write)

    std::thread io_thread_;
    std::thread watchdog_thread_;
};

} // namespace tt
