#pragma once
// IBKR broker adapter via the Client Portal Web API. Orders route to a
// locally running Client Portal Gateway (Java, listens on
// https://localhost:5000) which holds the brokerage session; the user logs
// in once via browser and this adapter keeps the session alive with
// periodic /tickle calls.
//
// Chosen over the TWS socket protocol for v1 because it reuses the proven
// REST/JSON plumbing (curl + parsers)
// and every response parser is unit-testable offline. The TWS-socket / FIX
// route is the later upgrade if unattended operation or lower order-path
// latency demands it.
//
// Threading contract: engine-thread calls enqueue
// commands (SPSC ring + wake pipe) and return immediately; one I/O thread
// owns all networking; fills/cancels/rejects come back via poll_event().
// Order status is polled (~1 s) and executions come from
// /iserver/account/trades, deduped by execution id — push updates via the
// gateway websocket are a future refinement.

#include "engine/broker.h"
#include "engine/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tt {

struct IbkrConfig {
    // Client Portal Gateway base URL. Loopback by default (127.0.0.1, not
    // localhost — the gateway's Jetty often binds IPv4 only); its self-signed
    // TLS cert is accepted only because the hop never leaves the machine.
    std::string gateway_url = "https://127.0.0.1:5000/v1/api";
    std::vector<std::string> symbols;   // session symbol table: id = index + 1
    // Optional per-symbol sub-account id (parallel to symbols). Empty entry =
    // route that symbol's orders to the session's primary account.
    std::vector<std::string> symbol_accounts;
    // When true, submit() refuses every order (no order ever reaches the
    // gateway). Set for accounts flagged read-only — a live login used only for
    // viewing/testing. Cancels/flatten are still allowed so positions can be
    // closed. Belt-and-suspenders alongside IBKR's server-side Read-Only API.
    bool read_only = false;
};

// ---- response parsers (exposed for unit tests) ----------------------------

// POST /iserver/account/{acct}/orders response: either an accepted order or
// a confirmation question that must be answered via /iserver/reply/{id}.
struct IbkrOrderResp {
    std::string order_id;    // set on acceptance
    std::string reply_id;    // set when the gateway asks a question
    std::string message;     // first question text / error, for logging
};
bool ibkr_parse_order_response(std::string_view json_text, IbkrOrderResp& out);

// GET /iserver/accounts -> selected (or first) account id, "" if none.
std::string ibkr_parse_first_account(std::string_view json_text);

// GET /iserver/accounts -> every tradeable (sub-)account id, in order.
std::vector<std::string> ibkr_parse_accounts(std::string_view json_text);

// GET /iserver/secdef/search?symbol=X -> conid of the US stock, 0 if absent.
int64_t ibkr_parse_conid(std::string_view json_text, const std::string& symbol);

// GET /iserver/account/orders
struct IbkrOrderStatus {
    std::string order_id;
    std::string status;   // PendingSubmit/Submitted/Filled/Cancelled/Inactive/...
};
bool ibkr_parse_orders(std::string_view json_text, std::vector<IbkrOrderStatus>& out);

// GET /iserver/account/trades
struct IbkrTrade {
    std::string execution_id;
    std::string order_ref;   // our cOID ("tt-...-N") when we set one
    std::string symbol;
    bool buy = true;
    double qty = 0, price = 0, commission = 0;
    int64_t ts_ms = 0;
};
bool ibkr_parse_trades(std::string_view json_text, std::vector<IbkrTrade>& out);

// GET /portfolio/{acct}/positions/0
struct IbkrPosition {
    int64_t conid = 0;
    double qty = 0;
};
bool ibkr_parse_positions(std::string_view json_text, std::vector<IbkrPosition>& out);

// ----------------------------------------------------------------------------

class IbkrBroker final : public IBrokerAdapter {
public:
    explicit IbkrBroker(IbkrConfig cfg);
    ~IbkrBroker() override;   // signals and joins the I/O thread

    uint64_t submit(const OrderRequest& r, int64_t now_ns) override;
    bool cancel(uint64_t order_id) override;
    void cancel_all() override;
    void flatten() override;
    bool poll_event(EngineEvent& out) override { return ev_ring_->try_pop(out); }
    bool ready() const override { return ready_.load(std::memory_order_acquire); }

    // Status/log lines (I/O thread produces, UI drains each frame).
    bool pop_log(std::string& out);

private:
    struct Cmd {
        enum : uint8_t { Submit = 1, Cancel, CancelAll, Flatten } type = Submit;
        uint64_t local_id = 0;
        OrderRequest req{};
    };
    struct Io;   // defined in ibkr_broker.cpp; owns all libcurl state

    void io_loop();
    void push_ev(const EngineEvent& ev);
    void push_reject(uint64_t local_id);
    void log(std::string line);
    bool push_cmd(const Cmd& c);

    IbkrConfig cfg_;
    std::string client_prefix_;   // "tt-<session epoch ms>-"

    using CmdRing = SpscRing<Cmd, 1 << 10>;
    using EvRing = SpscRing<EngineEvent, 1 << 12>;
    std::unique_ptr<CmdRing> cmd_ring_ = std::make_unique<CmdRing>();
    std::unique_ptr<EvRing> ev_ring_ = std::make_unique<EvRing>();

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};

    std::mutex log_mu_;
    std::deque<std::string> logs_;

    uintptr_t wake_tx_ = static_cast<uintptr_t>(-1);
    uintptr_t wake_rx_ = static_cast<uintptr_t>(-1);

    std::thread io_thread_;   // last member: starts in ctor, joined in dtor
};

} // namespace tt
