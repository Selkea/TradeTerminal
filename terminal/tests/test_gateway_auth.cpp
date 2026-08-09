// Gateway-authentication policy. On 2026-08-09 the 00:55 cold restart's
// automated re-login failed; IB Gateway sat on "UNRECOGNIZED USERNAME OR
// PASSWORD" for ~13 hours with its process alive and port 4002 listening, and
// nothing noticed. These cases pin the properties that make this class able to
// see that, which "is the process up / is the port open" structurally cannot:
//   - only a DATA FARM connecting (or bars arriving) counts as proof, because
//     neither can happen unless the gateway is logged in to IBKR, and
//   - proof belongs to one gateway SESSION and expires, so last night's login
//     can never vouch for this morning's.
#include "doctest.h"

#include "net/gateway_auth.h"

using namespace tt::net;

// Ages are ms off a steady clock; the tests hand in their own "now".
static constexpr int64_t kMin = 60'000;
static constexpr int64_t kHour = 60 * kMin;

// ---- the failure that motivated this ---------------------------------------

TEST_CASE("auth: a gateway that has said nothing is NOT authenticated") {
    // The incident state. The socket is up (that is why the class exists at
    // all), tws-data is connected, and not one farm message has arrived.
    GatewayAuth a;
    CHECK_FALSE(a.authed(0));
    CHECK_FALSE(a.authed(13 * kHour));   // ...still not, 13 hours later
    CHECK(a.proof_age_ms(13 * kHour) == -1);
    CHECK(a.farms_ok() == 0);
}

TEST_CASE("auth: the healthy log line makes it authenticated") {
    // Exactly what terminal.log shows on a good session, in order.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    CHECK(a.authed(0));
    a.farm_status(kFarmHmdsOk, 100);
    a.farm_status(kFarmSecDefOk, 200);
    CHECK(a.authed(200));
    CHECK(a.farms_ok() == 3);
    CHECK(a.proof_age_ms(1 * kMin) == 1 * kMin - 200);
}

TEST_CASE("auth: the 2026-08-09 night, replayed either way") {
    // Minutes from midnight. The gateway drops us when it restarts at 00:55, so
    // the previous session's proof is gone; what happens next is the whole test.
    const int64_t restart = 55 * kMin;
    GatewayAuth good, bad;
    good.farm_status(kFarmMktDataOk, 45 * kMin);   // yesterday's session
    bad.farm_status(kFarmMktDataOk, 45 * kMin);
    good.session_lost();
    bad.session_lost();
    // Login SUCCEEDS: farms connect a minute after the restart.
    good.farm_status(kFarmMktDataOk, restart + 1 * kMin);
    // Login FAILS: the socket comes back (port 4002 never stopped listening),
    // but no farm ever reports. `bad` gets nothing at all.
    const int64_t preopen = 8 * kHour + 45 * kMin;
    CHECK(good.authed(preopen));
    CHECK_FALSE(bad.authed(preopen));
    CHECK(bad.proof_age_ms(preopen) == -1);
}

// ---- staleness --------------------------------------------------------------

TEST_CASE("auth: a proof does not vouch for the gateway forever") {
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    CHECK(a.authed(kAuthProofMaxAgeMs));
    CHECK_FALSE(a.authed(kAuthProofMaxAgeMs + 1));
    // ...but the age keeps being reported, so /diag says HOW stale, not just
    // that it is: an operator has to be able to tell 13 hours from 13 minutes.
    CHECK(a.proof_age_ms(kAuthProofMaxAgeMs + 1) == kAuthProofMaxAgeMs + 1);
}

TEST_CASE("auth: the window must clear a healthy night") {
    // The farm messages are not a heartbeat — they arrive when a farm's state
    // changes and when a client connects. On a healthy night the freshest proof
    // is from just after the 23:55 cold restart, ~8h50m before the 08:45 check.
    // If the window did not clear that, this would page (and relaunch the
    // gateway, spending a login attempt) every single morning.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);                 // 23:57
    CHECK(a.authed(8 * kHour + 48 * kMin));           // 08:45
    CHECK(kAuthProofMaxAgeMs > 9 * kHour);
}

TEST_CASE("auth: a new gateway session throws the old proof away") {
    // The heart of it. A restart is exactly when a re-login can fail, so proof
    // from before the restart is worth nothing after it.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    CHECK(a.authed(1 * kMin));
    a.session_lost();
    CHECK_FALSE(a.authed(1 * kMin));
    CHECK(a.proof_age_ms(1 * kMin) == -1);
    CHECK(a.farms_ok() == 0);
    // The reconnect alone proves nothing — only the farms coming back do.
    a.farm_status(kFarmMktDataOk, 2 * kMin);
    CHECK(a.authed(2 * kMin));
}

// ---- partial farm sets ------------------------------------------------------

TEST_CASE("auth: one farm is enough - ushmds need never be seen") {
    // A farm cannot connect over an unauthenticated session, so usfarm alone
    // already answers the question this class is asked. Demanding all three
    // would page whenever IBKR takes a single farm down, which is not a login
    // failure — and the response to a page here is a gateway relaunch, i.e. a
    // login attempt, on an account IBKR locks after repeated failures.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    CHECK(a.authed(0));
    CHECK(a.farms_ok() == 1);   // ...but /diag still shows the set is partial
}

TEST_CASE("auth: any one of the three farms proves the login on its own") {
    for (const int code : {kFarmMktDataOk, kFarmHmdsOk, kFarmSecDefOk}) {
        GatewayAuth a;
        a.farm_status(code, 0);
        CHECK(a.authed(0));
        CHECK(a.farms_ok() == 1);
    }
}

TEST_CASE("auth: a farm going broken retires that farm's proof") {
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    a.farm_status(kFarmHmdsOk, 0);
    CHECK(a.farms_ok() == 2);
    a.farm_status(kFarmMktDataBroken, 1 * kMin);
    CHECK(a.farms_ok() == 1);
    CHECK(a.authed(1 * kMin));   // ushmds still up: still logged in
    a.farm_status(kFarmHmdsBroken, 2 * kMin);
    CHECK(a.farms_ok() == 0);
    CHECK_FALSE(a.authed(2 * kMin));   // nothing is connected to IBKR any more
}

TEST_CASE("auth: a farm that comes back clears the broken state") {
    GatewayAuth a;
    a.farm_status(kFarmMktDataBroken, 0);
    CHECK_FALSE(a.authed(0));
    a.farm_status(kFarmMktDataOk, 1 * kMin);
    CHECK(a.authed(1 * kMin));
    CHECK(a.farms_ok() == 1);
}

TEST_CASE("auth: the idle-farm notices are not evidence of anything") {
    // 2107/2108 ("connection is inactive but should be available upon demand")
    // is the NORMAL out-of-hours state of a logged-in gateway. Reading it as a
    // farm going down would make every healthy overnight gateway look logged
    // out at 08:45 — the false page this class must never produce.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    a.farm_status(2107, 1 * kMin);
    a.farm_status(2108, 2 * kMin);
    CHECK(a.authed(2 * kMin));
    CHECK(a.farms_ok() == 1);
    // ...and they cannot conjure a proof out of nothing either.
    GatewayAuth b;
    b.farm_status(2107, 0);
    b.farm_status(2108, 0);
    CHECK_FALSE(b.authed(0));
    CHECK(b.proof_age_ms(0) == -1);
}

// ---- the other two signals --------------------------------------------------

TEST_CASE("auth: delivered bars prove the login on their own") {
    // The signal that stays fresh through the trading day, after the session's
    // farm messages have long gone quiet. IB does not serve history to a
    // session that is not logged in.
    GatewayAuth a;
    a.bars_delivered(0);
    CHECK(a.authed(0));
    CHECK(a.farms_ok() == 0);   // no farm message needed
    CHECK(a.proof_age_ms(5 * kMin) == 5 * kMin);
    CHECK_FALSE(a.authed(kAuthProofMaxAgeMs + 1));   // and they go stale too
}

TEST_CASE("auth: bars keep the proof fresh after the farms have gone quiet") {
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    a.bars_delivered(6 * kHour);
    CHECK(a.authed(11 * kHour));            // bars are 5h old
    CHECK(a.proof_age_ms(11 * kHour) == 5 * kHour);
    // The farm OK on its own would have expired by now.
    CHECK(kAuthProofMaxAgeMs < 13 * kHour);
    CHECK(a.authed(13 * kHour));
}

TEST_CASE("auth: error 1100 means not authenticated until it clears") {
    // 1100 = the gateway lost ITS connection to IBKR. Our socket to the gateway
    // stays up, so nothing else here would notice. TwsBroker tracks the same
    // pair for the orders client, which is disconnected overnight by design.
    GatewayAuth a;
    a.farm_status(kFarmMktDataOk, 0);
    CHECK(a.authed(1 * kMin));
    a.upstream_lost();
    CHECK_FALSE(a.authed(1 * kMin));
    CHECK(a.proof_age_ms(1 * kMin) == 1 * kMin);   // the age is still reportable
    a.upstream_restored();                          // 1101/1102
    CHECK(a.authed(1 * kMin));
}

TEST_CASE("auth: a farm coming back also clears an unmatched 1100") {
    // A farm cannot connect over a dead IBKR link, so seeing one is proof the
    // 1102 we missed happened anyway. Without this, one dropped 1102 would leave
    // the terminal convinced the gateway was logged out until the next restart.
    GatewayAuth a;
    a.upstream_lost();
    a.farm_status(kFarmMktDataOk, 1 * kMin);
    CHECK(a.authed(1 * kMin));
    // Bars are equally conclusive.
    GatewayAuth b;
    b.upstream_lost();
    b.bars_delivered(1 * kMin);
    CHECK(b.authed(1 * kMin));
}

TEST_CASE("auth: 1101/1102 alone is not proof of a login") {
    // "Connectivity restored" without a farm or a bar says the gateway is
    // talking to IBKR, but nothing has actually been served over it yet. It
    // clears the 1100 and no more; a session that has never proven itself must
    // not become authenticated by a connectivity notice.
    GatewayAuth a;
    a.upstream_restored();
    CHECK_FALSE(a.authed(0));
    CHECK(a.proof_age_ms(0) == -1);
}
