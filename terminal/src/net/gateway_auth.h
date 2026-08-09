#pragma once

// Is IB Gateway actually LOGGED IN to IBKR? — tracked as state, not as a log
// line.
//
// 2026-08-09, a Sunday. At 00:55 the gateway did its scheduled cold restart,
// its automated re-login FAILED, and it sat on a modal dialog reading
// "UNRECOGNIZED USERNAME OR PASSWORD" for ~13 hours until a human looked at it.
// Nothing retried and nothing paged. The credentials were FINE — a manual login
// with the same ones worked first try; IBKR returns that message during its
// weekend maintenance window.
//
// Every signal we had was structurally incapable of seeing it:
//   - the gateway PROCESS was running and port 4002 was LISTENING all night.
//     Neither can tell "authenticated" from "sitting at a login-failure modal",
//     and those are exactly the two things that were checked.
//   - /diag broker_connected is false all night BY DESIGN (no live session, so
//     the orders client is deliberately disconnected rather than holding a TWS
//     client id), so it is not an overnight health signal.
//   - App::pump_broker_watchdog returns early on !live_running().
//   - the external keepalive only acts on live && !broker_connected, so
//     overnight it correctly sits still — and therefore cannot detect this.
// Net effect: a failed overnight re-login stays invisible until the 09:25
// auto-start brings up a session that cannot reach the market.
//
// What DOES prove authentication is already arriving on the tws-data client and
// was being thrown away: the data-farm status messages.
//   tws-data: gateway farm status 2104: Market data farm connection is OK:usfarm
//   tws-data: gateway farm status 2106: HMDS data farm connection is OK:ushmds
//   tws-data: gateway farm status 2158: Sec-def data farm connection is OK:secdefil
// A data farm cannot connect unless the gateway is logged in to IBKR, so ANY of
// those is positive proof — and none of them arrives at a gateway stuck on the
// login dialog. Successful bar deliveries prove the same thing.
//
// Same shape as net/hist_freshness.h (v0.12.0): header-only, mutex-guarded,
// clock injected by the caller so the policy is unit-testable without a gateway.

#include <array>
#include <cstdint>
#include <mutex>

namespace tt::net {

// Timestamps handed to this class must come from a STEADY clock, for the same
// reason bar freshness does: the VPS re-syncs NTP around the nightly gateway
// restart, and a wall-clock jump there would read as hours of fake staleness and
// page the phone at 08:45.

// The farm-status codes worth an opinion. IB reports each farm's state as a
// 21xx "error"; the OK/broken pairs below are the only ones that carry evidence.
inline constexpr int kFarmMktDataOk = 2104;    // "Market data farm connection is OK:usfarm"
inline constexpr int kFarmHmdsOk = 2106;       // "HMDS data farm connection is OK:ushmds"
inline constexpr int kFarmSecDefOk = 2158;     // "Sec-def data farm connection is OK:secdefil"
inline constexpr int kFarmMktDataBroken = 2103;   // "...is broken"
inline constexpr int kFarmHmdsBroken = 2105;
inline constexpr int kFarmSecDefBroken = 2157;
// Deliberately NOT handled: 2107/2108 ("...connection is inactive but should be
// available upon demand"). That is the NORMAL idle state of a logged-in gateway
// out of hours — treating it as a farm going down would make a healthy
// overnight gateway read as logged out and page every single morning.

// How long a proof stays good.
//
// The farm messages are not a heartbeat: IB sends them when a farm's state
// CHANGES and when a client connects, so on a healthy night the freshest proof
// is from just after the 23:55 cold restart — ~8h50m old by the 08:45 pre-open
// check. The window has to clear that comfortably or the check cries wolf every
// morning, and a false alarm here is not free: it relaunches the gateway, which
// spends a login attempt, and IBKR locks accounts on repeated failed logins.
//
// The window is the backstop, not the mechanism. The real scoping is the
// SESSION: session_lost() throws every proof away, so evidence never survives
// the socket drop that a gateway restart causes. The window only catches the
// residual case where the socket somehow stays up for more than half a day.
inline constexpr int64_t kAuthProofMaxAgeMs = 12 * 60 * 60 * 1000;

class GatewayAuth {
public:
    // I/O thread, on every 21xx farm-status message. Must be called for EVERY
    // one, not just the first of each code: tws_data's farm_codes_seen set is a
    // log-dedup that lives for the whole PROCESS and is not cleared on
    // reconnect, so a session's farm messages are logged once ever — driving
    // health off that set would make every session after the first look dead.
    void farm_status(int code, int64_t now_ms) {
        std::lock_guard<std::mutex> g(mu_);
        switch (code) {
        case kFarmMktDataOk:     set_farm(0, true, now_ms); break;
        case kFarmHmdsOk:        set_farm(1, true, now_ms); break;
        case kFarmSecDefOk:      set_farm(2, true, now_ms); break;
        case kFarmMktDataBroken: set_farm(0, false, now_ms); break;
        case kFarmHmdsBroken:    set_farm(1, false, now_ms); break;
        case kFarmSecDefBroken:  set_farm(2, false, now_ms); break;
        default: break;   // no evidence either way (see 2107/2108 above)
        }
    }

    // A history request that actually came back with bars. Independent proof:
    // IB does not serve history to a session that is not logged in. Kept as its
    // own signal because it is the one that keeps ticking through the day, long
    // after this session's farm messages have gone quiet.
    void bars_delivered(int64_t now_ms) {
        std::lock_guard<std::mutex> g(mu_);
        last_bars_ms_ = now_ms;
        upstream_lost_ = false;
    }

    // Error 1100: the gateway lost its own connection to IBKR. The socket to US
    // stays up, so nothing else here would notice — this is the same signal
    // TwsBroker::upstream_connected() keeps for the ORDERS client, which is
    // useless overnight because that client is deliberately disconnected.
    void upstream_lost() {
        std::lock_guard<std::mutex> g(mu_);
        upstream_lost_ = true;
    }

    // Error 1101/1102: connectivity restored (1101 = data lost, 1102 = data
    // maintained). Either way the gateway is talking to IBKR again, which it
    // could not do unauthenticated — but it says nothing about the farms, so it
    // only clears the 1100 rather than standing in as farm proof.
    void upstream_restored() {
        std::lock_guard<std::mutex> g(mu_);
        upstream_lost_ = false;
    }

    // The socket to the gateway went away. Proof belongs to a SESSION: a
    // gateway restart drops us, and the farm messages of the session before it
    // say nothing about whether the re-login succeeded — which is the entire
    // failure being guarded against. Cheap enough to call every reconnect poll.
    void session_lost() {
        std::lock_guard<std::mutex> g(mu_);
        farm_ok_.fill(false);
        last_farm_ok_ms_ = -1;
        last_bars_ms_ = -1;
        upstream_lost_ = false;
    }

    // The question the pre-open check asks: is the gateway logged in RIGHT NOW?
    //
    // Requires a farm that is currently OK, or a bar delivery — either one fresh
    // enough — with no unrecovered 1100. Deliberately ANY farm rather than all
    // three: one farm connecting already proves the login, whereas demanding the
    // full set would page (and relaunch the gateway, spending a login attempt)
    // whenever IBKR takes a single farm down, which is not a login failure at
    // all. How complete the set is gets reported separately, see farms_ok().
    bool authed(int64_t now_ms) const {
        std::lock_guard<std::mutex> g(mu_);
        if (upstream_lost_) return false;
        const bool farm_proof = any_farm_ok() && fresh(last_farm_ok_ms_, now_ms);
        return farm_proof || fresh(last_bars_ms_, now_ms);
    }

    // ms since the freshest proof of authentication, -1 if there has never been
    // one on this session. This is what /diag reports: on the incident night it
    // would have read -1 from 00:55 onwards, every minute, all night.
    int64_t proof_age_ms(int64_t now_ms) const {
        std::lock_guard<std::mutex> g(mu_);
        const int64_t newest = last_farm_ok_ms_ > last_bars_ms_ ? last_farm_ok_ms_
                                                                : last_bars_ms_;
        return newest < 0 ? -1 : now_ms - newest;
    }

    // How many of the three farms are currently reporting OK (0..3). Diagnostic
    // detail only — a partial set does NOT page (see authed()).
    int farms_ok() const {
        std::lock_guard<std::mutex> g(mu_);
        int n = 0;
        for (const bool ok : farm_ok_)
            if (ok) ++n;
        return n;
    }

private:
    // 0 = market data (usfarm), 1 = HMDS (ushmds), 2 = sec-def (secdefil).
    void set_farm(size_t i, bool ok, int64_t now_ms) {
        farm_ok_[i] = ok;
        if (!ok) return;
        last_farm_ok_ms_ = now_ms;
        // A farm cannot come up over a dead IBKR link, so an OK also clears a
        // 1100 whose matching 1102 we never saw.
        upstream_lost_ = false;
    }
    bool any_farm_ok() const {
        for (const bool ok : farm_ok_)
            if (ok) return true;
        return false;
    }
    static bool fresh(int64_t stamp_ms, int64_t now_ms) {
        return stamp_ms >= 0 && now_ms - stamp_ms <= kAuthProofMaxAgeMs;
    }

    mutable std::mutex mu_;
    std::array<bool, 3> farm_ok_{};   // current state per farm
    int64_t last_farm_ok_ms_ = -1;    // freshest farm OK on this session (-1 = none)
    int64_t last_bars_ms_ = -1;       // freshest successful bar delivery (-1 = none)
    bool upstream_lost_ = false;      // 1100 with no 1101/1102 since
};

} // namespace tt::net
