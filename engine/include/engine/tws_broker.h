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
    bool ready() const override { return ready_.load(std::memory_order_acquire); }

    // Status/log lines (I/O thread produces, UI drains each frame).
    bool pop_log(std::string& out);

    // Measured order-path latency (submit -> first ack), for the fill sim.
    AckSummary ack_latency() const { return ack_lat_.summary(); }

private:
    struct Cmd {
        enum : uint8_t { Submit = 1, Cancel, CancelAll, Flatten } type = Submit;
        uint64_t local_id = 0;
        OrderRequest req{};
    };
    struct Io;   // defined in tws_broker.cpp; owns all TWS API state

    void io_loop();
    void push_ev(const EngineEvent& ev);
    // Record a reject reason (I/O thread) then push the Rejected event. code 0
    // and empty msg = no reason available (leaves the reason table untouched).
    // symbol_id + protective mark a rejected protective stop leg so the engine
    // can flatten the position it was guarding (see kEvFlagProtective).
    void push_reject(uint64_t local_id, int code = 0, std::string msg = {},
                     uint32_t symbol_id = 0, bool protective = false);
    void log(std::string line);
    bool push_cmd(const Cmd& c);

    TwsConfig cfg_;

    using CmdRing = SpscRing<Cmd, 1 << 10>;
    using EvRing = SpscRing<EngineEvent, 1 << 12>;
    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::unique_ptr<EvRing> ev_ring_ = std::make_unique<EvRing>();

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
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

    AckLatency ack_lat_;   // recorded on the I/O thread, read from the UI thread

    std::thread io_thread_;
};

} // namespace tt
