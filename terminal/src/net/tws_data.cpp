#include "net/tws_data.h"

#include "net/bar_cache.h"
#include "net/hist_pacing.h"
#include "net/hist_requests.h"

#include "Contract.h"          // Contract + ContractDetails
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "ScannerSubscription.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

namespace tt::net {

namespace {

// Streaming quote tickerIds live far above the candle request ids (which are
// the public next_id_ values) so the two spaces can never collide.
constexpr int kQuoteBase = 500'000;

// One-shot market scans use a single fixed tickerId (only one scan runs at a
// time), between the candle id space and the quote base so it collides with
// neither.
constexpr int kScanReqId = 400'000;

// A history fetch outstanding longer than this means the data session has gone
// half-open: the socket is up and streaming quotes keep flowing ("data
// maintained"), but reqHistoricalData answers are silently dropped — the state
// the nightly IB Gateway restart (error 1100 -> 1102) can leave behind. A
// normal 1y/1h fetch answers in a few seconds, so 20s is unambiguous. On this
// we force a data-session reconnect to clear it; the cooldown stops a genuinely
// unrecoverable failure from thrashing the quotes that share the session.
// kHistTimeoutMs, kBigBatchBars and the send/retry predicates live in
// net/hist_pacing.h, with the measurements that justify them.
constexpr auto kHistResetCooldown = std::chrono::seconds(60);

// TickType ids on the reqMktData stream (live + delayed variants).
constexpr int kTickBid = 1, kTickDelayedBid = 66;
constexpr int kTickAsk = 2, kTickDelayedAsk = 67;
constexpr int kTickLast = 4, kTickDelayedLast = 68;
constexpr int kTickVolume = 8, kTickDelayedVolume = 74;

const char* tws_bar_size(const std::string& interval) {
    if (interval == "1s") return "1 secs";
    if (interval == "1m") return "1 min";
    if (interval == "2m") return "2 mins";
    if (interval == "5m") return "5 mins";
    if (interval == "15m") return "15 mins";
    if (interval == "30m") return "30 mins";
    if (interval == "1h") return "1 hour";
    if (interval == "1d") return "1 day";
    return nullptr;
}

// Duration ladder, smallest to largest; index doubles as a comparable rank.
struct Dur {
    const char* range;
    const char* dur;
};
constexpr Dur kDurs[] = {{"1m", "60 S"},   {"5m", "300 S"}, {"15m", "900 S"},
                         {"30m", "1800 S"}, {"1h", "3600 S"},
                         {"1d", "1 D"},    {"5d", "5 D"},   {"1mo", "1 M"},
                         {"6mo", "6 M"},   {"1y", "1 Y"},   {"2y", "2 Y"},
                         {"5y", "5 Y"},    {"max", "15 Y"}};

int dur_idx(const std::string& range) {
    for (int i = 0; i < static_cast<int>(std::size(kDurs)); ++i)
        if (range == kDurs[i].range) return i;
    return -1;
}

// IB caps how much history one request may span per bar size; clamp so a
// "1m x 5y" chart still shows the most recent stretch instead of erroring.
int max_dur_idx(const std::string& interval) {
    if (interval == "1s") return dur_idx("30m");           // ~1800 1-sec bars
    if (interval == "1m" || interval == "2m") return dur_idx("1mo");
    if (interval == "5m" || interval == "15m" || interval == "30m")
        return dur_idx("6mo");
    if (interval == "1h") return dur_idx("2y");
    return static_cast<int>(std::size(kDurs)) - 1;         // daily: anything
}

// formatDate=2 gives epoch seconds for intraday bars but "YYYYMMDD" for
// daily bars; normalize both to epoch seconds (daily at UTC midnight).
int64_t parse_bar_time(const std::string& t) {
    if (t.size() == 8 && t.find_first_not_of("0123456789") == std::string::npos) {
        std::tm tm{};
        tm.tm_year = std::stoi(t.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(t.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(t.substr(6, 2));
#ifdef _WIN32
        return static_cast<int64_t>(_mkgmtime(&tm));
#else
        return static_cast<int64_t>(timegm(&tm));
#endif
    }
    try {
        return std::stoll(t);
    } catch (...) {
        return 0;
    }
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

// Monotonic milliseconds for GatewayAuth's ages. Steady, not wall clock: the
// VPS re-syncs NTP around the nightly gateway restart, and a jump there would
// read as hours of fake staleness on the 08:45 pre-open check.
int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// The identity of a historical request, for BOTH the bar cache and the pacing
// gate. One function so the two can never disagree about what "the same
// request" means — IB's 15-second identical-request rule and the cache's
// "already have this series" answer have to be about the same thing.
// '\x1f' (unit separator) is not legal in a ticker, an interval or a range.
std::string req_key(const std::string& symbol, const std::string& interval,
                    const std::string& range) {
    return symbol + '\x1f' + interval + '\x1f' + range;
}

} // namespace

struct TwsData::Io final : DefaultEWrapper {
    TwsData& d;
    EReaderOSSignal signal;
    std::unique_ptr<EClientSocket> client;
    std::unique_ptr<EReader> reader;

    // In-flight candle fetches. The TWS reqId is NO LONGER the public request
    // id: a retry re-issues under a fresh id and HistPending::pub_id carries the
    // caller's id across the swap. See net/hist_requests.h for why (a retry that
    // reused its own id was destroyed by its own cancel acknowledgement).
    HistRequests hist;
    // Delivered series, replayed instead of re-fetched (net/bar_cache.h). This
    // is the single biggest lever on the daily lineup build: 97.3% of the
    // 2026-08-10 build's 1543 s was dead IO wait, and a tournament's five
    // candidates were each fetching the same symbol's same series.
    BarCache bars;
    // Aggregate send rate (net/hist_pacing.h): minimum spacing, IB's 15s
    // identical-request rule, and the rolling 60-per-10-minutes budget the same
    // build exhausted.
    HistSendGate gate;
    // Request keys we have already logged a pacing hold for. pump_requests runs
    // every io_loop pass (~1/s) and a budget hold can last minutes, so without
    // this one held request would write hundreds of identical log lines.
    std::unordered_set<std::string> held_logged_;
    // Distinct farm-status codes already reported (the stream repeats them).
    std::unordered_set<int> farm_codes_seen;
    // When the last historicalDataEnd landed, and how big it was. IB stops
    // answering a request issued too soon after a large delivery — see
    // kBigBatchBars.
    std::chrono::steady_clock::time_point last_hist_end{};
    size_t last_hist_bars = 0;

    // Issue (or re-issue) one history request. Kept in one place so the retry
    // path below is byte-identical to the original send.
    void send_history(int id, HistPending& p) {
        p.candles.clear();
        p.sent_ms = steady_ms();
        client->reqHistoricalData(static_cast<TickerId>(id), stock(p.symbol), "",
                                  p.dur, p.bar, "TRADES",
                                  /*useRTH=*/1, /*formatDate=*/2,
                                  /*keepUpToDate=*/false, TagValueListSPtr());
    }

    // A history request that blew the timeout is almost never a broken session:
    // measurement showed it is one request IB silently declined to answer while
    // its siblings on the SAME socket succeeded. Cancel and re-issue just that
    // one. Tearing the session down (drop_connection) also destroys and
    // re-subscribes every quote stream, ~75 times a day, and re-queues the very
    // requests that were already in flight — which is why the same batch was
    // being fetched over and over.
    //
    // The re-issue takes a NEW reqId off the same monotonic counter the public
    // ids come from. Re-using the dead id (0.13.x-0.14.1) meant IB's cancel
    // acknowledgement — error 162, delivered on the NEXT processMsgs — matched
    // the retry and erased it, so the retry could never be seen to succeed and
    // the escalation below could never fire. That silently replaced a recovery
    // that had worked 286 times with no recovery at all. See
    // net/hist_requests.h.
    bool retry_dead_history(int64_t now_ms) {
        bool retried_any = false;
        // The retry goes out through the same socket as a first send, so it is
        // subject to the same pacing rule pump_requests enforces below — see
        // HistRequests::dead_ids for why sending into the quiet window would now
        // cost a whole data session rather than one silently dropped request.
        const auto since_end = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - last_hist_end)
                                   .count();
        const bool blocked = history_send_blocked(last_hist_bars, since_end);
        for (const int id : hist.dead_ids(now_ms, blocked)) {
            const HistPending* dead = hist.find(id);
            if (!dead) continue;
            // A retry is a send, so it spends the same budget and obeys the same
            // minimum spacing as a first issue (net/hist_pacing.h). It cannot be
            // held by the 15s identical rule — the request it repeats went out at
            // least kHistTimeoutMs (20s) ago — but it CAN be held by a budget the
            // build has already spent, which is precisely when re-sending would
            // be the thing that keeps the session silent.
            const std::string gkey = req_key(dead->symbol, dead->interval, dead->range);
            if (!gate.try_send(gkey, now_ms)) continue;
            const int64_t age = now_ms - dead->sent_ms;
            const int new_id =
                static_cast<int>(d.next_id_.fetch_add(1, std::memory_order_relaxed));
            HistPending* p = hist.begin_retry(id, new_id, now_ms);
            if (!p) continue;
            retried_any = true;
            // Cancel FIRST, then send: the ack for `id` is now owed to the
            // awaiting-ack set, not to the request going out on `new_id`.
            if (client) client->cancelHistoricalData(static_cast<TickerId>(id));
            d.log(p->symbol + " " + p->interval + ": history unanswered for " +
                  std::to_string(age / 1000) + "s - cancelled req " +
                  std::to_string(id) + ", retrying as req " +
                  std::to_string(new_id));
            if (client) send_history(new_id, *p);
        }
        return retried_any;
    }

    // Active quote streams: symbol -> tickerId + last known quote state.
    struct Stream {
        int req_id = 0;
        int mkt_type = 0;   // last marketDataType (1 rt / 3 delayed); 0 = unknown
        Quote q;
    };
    std::unordered_map<std::string, Stream> streams;
    int next_quote_req = kQuoteBase;

    // The one in-flight market scan. Streaming subscriptions keep updating, so
    // scannerDataEnd cancels it to take a single snapshot; cb then fires once.
    struct ActiveScan {
        bool running = false;
        TwsData::ScanCb cb;
        std::vector<ScanHit> hits;
    };
    ActiveScan scan;

    // Handshake rejected (e.g. paper disclaimer not accepted yet): the socket
    // stays open but no session will come; io_loop tears down and retries.
    // Set inside EWrapper callbacks, where destroying the reader is unsafe.
    bool reset_conn = false;

    explicit Io(TwsData& data) : d(data), signal(1000) {}

    bool connect_gateway() {
        client = std::make_unique<EClientSocket>(this, &signal);
        if (!client->eConnect(d.host_.c_str(), d.port_, d.client_id_)) {
            client.reset();
            return false;
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        d.log("connecting to IB Gateway at " + d.host_ + ":" +
              std::to_string(d.port_));
        return true;
    }

    void drop_connection() {
        d.connected_.store(false, std::memory_order_release);
        // Login proof belongs to the SESSION that produced it. A gateway
        // restart drops us, and last night's farm messages say nothing about
        // whether tonight's re-login worked — which is the exact failure the
        // pre-open check exists for.
        d.auth_.session_lost();
        if (client) client->eDisconnect();
        reader.reset();
        client.reset();
        // In-flight history requests will never answer; unblock the waiters.
        // Reported under pub_id, not the map key: after a retry the key is an
        // id the caller was never given. clear() also drops any cancelled id
        // still awaiting its acknowledgement — the socket that owed it is gone,
        // so holding the entry would only leak it across the reconnect.
        for (const auto& [id, p] : hist.live())
            if (d.cbs_.on_error)
                d.cbs_.on_error(p.pub_id, "tws",
                                "connection lost fetching " + p.symbol);
        hist.clear();
        // Cached bars belong to the SESSION that delivered them (see
        // kBarCacheTtlMs), the same rule auth_.session_lost() above follows: the
        // reason we are here is usually that history stopped being answered, and
        // replaying that session's last answers across the reconnect would hide
        // exactly the failure the reconnect exists to clear. The send budget goes
        // with it — it is IB's accounting for a client that no longer exists.
        bars.clear();
        gate.session_lost();
        held_logged_.clear();
        // An in-flight scan will never complete; deliver empty so the caller
        // (which is waiting on its callback) isn't left hanging.
        if (scan.running) {
            auto cb = std::move(scan.cb);
            scan = ActiveScan{};
            if (cb) cb({});
        }
        streams.clear();   // re-established on reconnect via want_dirty_
    }

    static Contract stock(const std::string& sym) {
        Contract c;
        c.symbol = sym;
        c.secType = "STK";
        c.exchange = "SMART";
        c.currency = "USD";
        return c;
    }

    // Drain UI-thread requests; only when a session is up so nothing is lost.
    void pump_requests() {
        if (!client || !d.connected_.load(std::memory_order_acquire)) return;
        std::vector<CandleReq> reqs;
        std::vector<std::string> want;
        bool dirty = false;
        ScanReq scan_req;
        bool have_scan = false;
        {
            std::lock_guard lock(d.mu_);
            reqs.swap(d.reqs_);
            if (d.want_dirty_) {
                want = d.want_syms_;
                d.want_dirty_ = false;
                dirty = true;
            }
            if (d.scan_pending_) {
                scan_req = std::move(d.scan_req_);
                d.scan_pending_ = false;
                have_scan = true;
            }
        }
        // Requests we could not send on this pass. Held individually rather than
        // "everything from here on", so one series waiting out IB's 15s
        // identical-request window cannot head-of-line-block a different symbol
        // that is free to go.
        std::vector<CandleReq> deferred;
        for (size_t ri = 0; ri < reqs.size(); ++ri) {
            const CandleReq& r = reqs[ri];
            const char* bar = tws_bar_size(r.interval);
            int di = dur_idx(r.range);
            if (!bar || di < 0) {
                if (d.cbs_.on_error)
                    d.cbs_.on_error(r.id, "tws",
                                    "cannot fetch " + r.symbol + " " + r.interval);
                continue;
            }
            const int64_t now_steady = steady_ms();
            const std::string gkey = req_key(r.symbol, r.interval, r.range);
            // Already have it: answer without touching the wire. This is what
            // turns a tournament's five candidate fetches of one symbol's one
            // series into one fetch (net/bar_cache.h). Delivered on THIS thread,
            // through the same callback a real delivery uses, under the caller's
            // own request id — so the routing in App is identical either way.
            if (const std::vector<Candle>* hit =
                    bars.get(r.symbol, r.interval, r.range, now_steady)) {
                CandleBatch b;
                b.id = r.id;
                b.symbol = r.symbol;
                b.interval = r.interval;
                b.cached = true;   // NOT proof the session works — see App::on_candles
                b.candles = *hit;
                d.log(r.symbol + " " + r.interval + " " + r.range + ": served " +
                      std::to_string(b.candles.size()) +
                      " bars from cache, no IB request (cache " +
                      std::to_string(bars.hits()) + " hit / " +
                      std::to_string(bars.misses()) + " miss)");
                if (d.cbs_.on_candles) d.cbs_.on_candles(std::move(b));
                continue;
            }
            // Quiet window after a large delivery (see kBigBatchBars): sending
            // into it is what gets a request silently dropped. Put the rest back
            // and come round again — the io_loop re-enters within ~1s. This one
            // IS all-or-nothing: the window is a property of the socket, not of
            // the request, so nothing queued behind it can go either.
            const auto since_end = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - last_hist_end)
                                       .count();
            if (history_send_blocked(last_hist_bars, since_end)) {
                deferred.insert(deferred.end(),
                                reqs.begin() + static_cast<ptrdiff_t>(ri), reqs.end());
                break;   // NOT return: the stream/scan work below was already drained
            }
            // Aggregate rate (net/hist_pacing.h). The 2026-08-10 build put 30
            // requests on the wire inside one second and spent IB's 60-per-10
            // -minutes budget before the tournaments had started.
            const SendHold hold = gate.hold(gkey, now_steady);
            if (hold != SendHold::None) {
                // Logged once per hold, not once per pass: the io_loop re-enters
                // roughly every second, and the Budget case can persist for
                // minutes. Only Identical and Budget are worth a line — MinGap is
                // the ordinary 500 ms spacing and would say nothing.
                // The dedup set is emptied on a session teardown and on each
                // successful send, so it tracks only currently-held keys — but
                // a request abandoned while held would leave one behind, so cap
                // it rather than trust that.
                if (held_logged_.size() > 512) held_logged_.clear();
                if (hold != SendHold::MinGap && held_logged_.insert(gkey).second)
                    d.log(r.symbol + " " + r.interval + " " + r.range + ": holding "
                          "the request back - " +
                          (hold == SendHold::Identical
                               ? "identical request sent less than 15s ago (IB pacing "
                                 "rule)"
                               : "already used " +
                                     std::to_string(gate.window_sends(now_steady)) +
                                     " of IB's 60 historical requests per 10 minutes"));
                deferred.push_back(r);
                continue;
            }
            gate.record(gkey, now_steady);
            held_logged_.erase(gkey);
            const int cap = max_dur_idx(r.interval);
            if (di > cap) {
                d.log(r.symbol + ": " + r.range + " of " + r.interval +
                      " bars exceeds IB's history window - clamped to " +
                      kDurs[cap].dur);
                di = cap;
            }
            // di is clamped above (e.g. 1-sec bars cap at "30m" = 1800 bars,
            // the most IB returns per request); the live tail extends it forward.
            const char* dur = kDurs[di].dur;
            // First issue: the TWS reqId starts out equal to the public id, and
            // first_sent_ms is pinned here so /diag reports the whole outage
            // rather than the age of the current attempt.
            const int req_id = static_cast<int>(r.id);
            HistPending p;
            p.pub_id = r.id;
            p.symbol = r.symbol;
            p.interval = r.interval;
            p.range = r.range;   // the cache key's third component
            p.dur = dur;
            p.bar = bar;
            p.sent_ms = p.first_sent_ms = steady_ms();
            hist.add(req_id, std::move(p));
            d.log(r.symbol + " " + r.interval + " " + r.range +
                  ": fetching from IB (cache miss, req " + std::to_string(req_id) +
                  ", " + std::to_string(gate.window_sends(steady_ms())) + "/" +
                  std::to_string(kHistMaxPerWindow) + " in the 10-minute budget)");
            send_history(req_id, *hist.find(req_id));
        }
        if (!deferred.empty()) {
            std::lock_guard lock(d.mu_);
            d.reqs_.insert(d.reqs_.begin(), deferred.begin(), deferred.end());
        }
        if (dirty) {
            for (auto it = streams.begin(); it != streams.end();) {
                if (std::find(want.begin(), want.end(), it->first) == want.end()) {
                    client->cancelMktData(it->second.req_id);
                    it = streams.erase(it);
                } else {
                    ++it;
                }
            }
            for (const std::string& s : want) {
                if (streams.count(s)) continue;
                Stream st;
                st.req_id = next_quote_req++;
                streams.emplace(s, st);
                client->reqMktData(st.req_id, stock(s), "", false, false,
                                   TagValueListSPtr());
            }
        }
        if (have_scan) {
            if (scan.running) client->cancelScannerSubscription(kScanReqId);
            ScannerSubscription sub;
            sub.instrument = scan_req.spec.instrument;
            sub.locationCode = scan_req.spec.location;
            sub.scanCode = scan_req.spec.scan_code;
            sub.numberOfRows = scan_req.spec.rows;
            if (scan_req.spec.price_above > 0) sub.abovePrice = scan_req.spec.price_above;
            if (scan_req.spec.volume_above > 0)
                sub.aboveVolume = static_cast<int>(scan_req.spec.volume_above);
            scan.running = true;
            scan.cb = std::move(scan_req.cb);
            scan.hits.clear();
            client->reqScannerSubscription(kScanReqId, sub, TagValueListSPtr(),
                                           TagValueListSPtr());
            d.log("scan: " + scan_req.spec.scan_code + " on " +
                  scan_req.spec.location);
        }
    }

    // ---- EWrapper -----------------------------------------------------------
    void nextValidId(OrderId) override {
        // Delayed data is fine when the account has no real-time subscription
        // (type 3 = delayed-if-needed); applies to history + streams alike.
        if (client) client->reqMarketDataType(3);
        {
            std::lock_guard lock(d.mu_);
            d.want_dirty_ = true;   // fresh session: re-establish every stream
        }
        if (!d.connected_.exchange(true, std::memory_order_acq_rel)) {
            d.conn_gen_.fetch_add(1, std::memory_order_relaxed);
            d.log("session up (IB Gateway " + d.host_ + ":" +
                  std::to_string(d.port_) + ")");
        }
    }

    void managedAccounts(const std::string& list) override {
        std::vector<std::string> all;
        std::string cur;
        for (const char ch : list + ",") {
            if (ch == ',') {
                if (!cur.empty()) all.push_back(cur);
                cur.clear();
            } else if (ch != ' ') {
                cur += ch;
            }
        }
        // Paper account codes start with 'D' (DU/DF prefixes).
        AccountKind kind = AccountKind::Unknown;
        if (!all.empty())
            kind = all[0].front() == 'D' ? AccountKind::Paper : AccountKind::Live;
        d.account_kind_.store(kind, std::memory_order_release);
        std::lock_guard lock(d.mu_);
        d.account_ = all.empty() ? "" : all[0];
        d.accounts_ = std::move(all);
    }

    void connectionClosed() override {
        d.connected_.store(false, std::memory_order_release);
        d.account_kind_.store(AccountKind::Unknown, std::memory_order_release);
        d.auth_.session_lost();   // see drop_connection
        d.log("connection closed");
    }

    void error(int id, int errorCode, const std::string& errorString,
               const std::string&) override {
        // Gateway<->IBKR connectivity: 1100 = lost, 1101/1102 = restored. Same
        // pair TwsBroker tracks for the ORDERS client — which is deliberately
        // disconnected outside a live session, so overnight this data client is
        // the only one in a position to see it.
        // Not logged here: these fall through to the generic error line below,
        // which already carries IB's own wording verbatim.
        if (errorCode == 1100) d.auth_.upstream_lost();
        else if (errorCode == 1101 || errorCode == 1102) d.auth_.upstream_restored();
        // Farm status (2100-2170) is routine chatter, but this used to return
        // BEFORE the hist lookup below — so any such message carrying a live
        // history reqId was swallowed whole and left that request Pending
        // forever with nothing in the log. Report each distinct code once (the
        // stream repeats them constantly), and never erase on one: 2106/2158 are
        // "connection is OK" notices and killing a healthy fetch on those would
        // be worse than the silence. A genuinely dead request is caught by the
        // timeout + retry path instead.
        if (errorCode >= 2100 && errorCode <= 2170) {
            // BEFORE the dedup, always: a farm connecting is the only proof we
            // get that the gateway is logged in to IBKR, and farm_codes_seen is
            // a process-lifetime log filter (drop_connection does not clear it),
            // so every session after the first would otherwise contribute no
            // evidence at all. See net/gateway_auth.h for the 2026-08-09 outage
            // this exists for.
            d.auth_.farm_status(errorCode, steady_ms());
            if (farm_codes_seen.insert(errorCode).second)
                d.log("gateway farm status " + std::to_string(errorCode) + ": " +
                      errorString);
            if (const HistPending* p = hist.find(id))
                d.log("...that arrived on in-flight history request " +
                      std::to_string(id) + " (" + p->symbol + ")");
            return;
        }
        if (errorCode == 10141) {
            // Paper disclaimer dialog not accepted yet — IBC clicks it a
            // couple of seconds after login, so connecting right after the
            // gateway boots races it. Back off and reconnect.
            d.log("gateway still accepting the paper disclaimer - retrying shortly");
            reset_conn = true;
            return;
        }
        // A request WE cancelled to retry it. IB answers cancelHistoricalData
        // with error 162 ("API historical data query cancelled: N") one
        // processMsgs later; before 0.15.0 the retry reused N, so this line
        // erased the retry and reported it to the caller as a feed error — 512
        // times on 2026-08-10. The ack belongs to the OLD request, which is
        // already gone from the live set, so absorb it silently: the "cancelled
        // req N, retrying as req M" line above already said what happened.
        if (hist.take_cancel_ack(id)) return;
        if (const HistPending* p = hist.find(id)) {
            if (d.cbs_.on_error)
                d.cbs_.on_error(p->pub_id, "tws", p->symbol + ": " + errorString);
            hist.erase(id);
            return;
        }
        if (scan.running && id == kScanReqId) {
            // Scan rejected (bad scanCode, no scanner entitlement, etc.):
            // deliver empty so the caller stops waiting.
            d.log("scan failed: " + errorString);
            auto cb = std::move(scan.cb);
            scan = ActiveScan{};
            if (cb) cb({});
            return;
        }
        d.log("error " + std::to_string(errorCode) + " (id " + std::to_string(id) +
              "): " + errorString);
    }

    void historicalData(TickerId reqId, const ::Bar& bar) override {
        // Unknown id = a late bar for a request we already cancelled; drop it.
        // A bar for the RETRY's id lands here normally now, which is the whole
        // point: before 0.15.0 the retry had been erased by its own cancel ack,
        // so its bars were discarded and the app could not tell the retry had
        // worked.
        HistPending* p = hist.find(static_cast<int>(reqId));
        if (!p) return;
        Candle c;
        c.ts = parse_bar_time(bar.time);
        c.open = bar.open;
        c.high = bar.high;
        c.low = bar.low;
        c.close = bar.close;
        c.volume = DecimalFunctions::decimalToDouble(bar.volume);
        if (c.volume < 0) c.volume = 0;   // -1 = not reported for this bar
        p->candles.push_back(c);
    }

    void historicalDataEnd(int reqId, const std::string&,
                           const std::string&) override {
        HistPending* p = hist.find(reqId);
        if (!p) return;
        CandleBatch b;
        b.id = p->pub_id;   // the id the CALLER asked under, not the retry's
        b.symbol = p->symbol;
        b.interval = p->interval;
        b.candles = std::move(p->candles);
        // Fill the cache under the caller's OWN range, which the request has
        // carried since it was queued. The next consumer of this exact series —
        // the tournament's next candidate, four of which asked for it four times
        // on 2026-08-10 — is answered from here instead of from IB.
        // put() ignores an empty delivery; see net/bar_cache.h.
        bars.put(p->symbol, p->interval, p->range, b.candles, steady_ms());
        hist.erase(reqId);
        // Remember how much just landed: the next request must not follow a big
        // batch too closely (see kBigBatchBars).
        last_hist_end = std::chrono::steady_clock::now();
        last_hist_bars = b.candles.size();
        // Bars only come back over an authenticated session, so a NON-EMPTY
        // delivery is independent proof the gateway is logged in — and it is the
        // proof that stays fresh during the day, after this session's farm
        // messages have gone quiet. An empty batch proves nothing.
        if (!b.candles.empty()) d.auth_.bars_delivered(steady_ms());
        if (d.cbs_.on_candles) d.cbs_.on_candles(std::move(b));
    }

    void tickPrice(TickerId tickerId, TickType field, double price,
                   const TickAttrib&) override {
        if (price <= 0.0) return;
        const int ft = static_cast<int>(field);
        const bool last = ft == kTickLast || ft == kTickDelayedLast;
        const bool bid = ft == kTickBid || ft == kTickDelayedBid;
        const bool ask = ft == kTickAsk || ft == kTickDelayedAsk;
        if (!last && !bid && !ask) return;
        for (auto& [sym, st] : streams) {
            if (st.req_id != static_cast<int>(tickerId)) continue;
            if (last) st.q.price = price;
            else if (bid) st.q.bid = price;
            else st.q.ask = price;
            st.q.ts_ms = now_ms();
            if (d.cbs_.on_tick) d.cbs_.on_tick(sym, st.q);
            break;
        }
    }

    // With reqMarketDataType(3) the gateway answers per stream whether it
    // granted real-time or delayed data — the subscription litmus test.
    void marketDataType(TickerId reqId, int marketDataType) override {
        static constexpr const char* kNames[] = {
            "?", "REAL-TIME", "frozen", "DELAYED (15 min)", "delayed-frozen"};
        for (auto& [sym, st] : streams) {
            if (st.req_id != static_cast<int>(reqId)) continue;
            if (st.mkt_type == marketDataType) return;
            st.mkt_type = marketDataType;
            const char* name =
                marketDataType >= 1 && marketDataType <= 4 ? kNames[marketDataType]
                                                           : kNames[0];
            d.log(sym + ": " + name + " market data");
            return;
        }
    }

    void tickSize(TickerId tickerId, TickType field, Decimal size) override {
        const int ft = static_cast<int>(field);
        if (ft != kTickVolume && ft != kTickDelayedVolume) return;
        for (auto& [sym, st] : streams) {
            if (st.req_id != static_cast<int>(tickerId)) continue;
            st.q.day_volume = DecimalFunctions::decimalToDouble(size);
            break;
        }
    }

    void scannerData(int reqId, int rank, const ContractDetails& cd,
                     const std::string& /*distance*/, const std::string& /*benchmark*/,
                     const std::string& /*projection*/,
                     const std::string& /*legsStr*/) override {
        if (!scan.running || reqId != kScanReqId) return;
        ScanHit h;
        h.symbol = cd.contract.symbol;
        h.exchange = !cd.contract.primaryExchange.empty() ? cd.contract.primaryExchange
                                                          : cd.contract.exchange;
        h.rank = rank;
        scan.hits.push_back(std::move(h));
    }

    void scannerDataEnd(int reqId) override {
        if (!scan.running || reqId != kScanReqId) return;
        // Snapshot taken: cancel the (streaming) subscription and hand results
        // to the waiting caller exactly once.
        if (client) client->cancelScannerSubscription(kScanReqId);
        auto cb = std::move(scan.cb);
        auto hits = std::move(scan.hits);
        scan = ActiveScan{};
        d.log("scan: " + std::to_string(hits.size()) + " hits");
        if (cb) cb(std::move(hits));
    }
};

// ---- adapter -------------------------------------------------------------

TwsData::~TwsData() { stop(); }

void TwsData::set_endpoint(std::string host, int port, int client_id) {
    host_ = std::move(host);
    port_ = port;
    client_id_ = client_id;
}

void TwsData::start(Callbacks cbs) {
    if (io_thread_.joinable()) return;
    cbs_ = std::move(cbs);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this] { io_loop(); });
}

void TwsData::stop() {
    running_.store(false, std::memory_order_release);
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_release);
}

uint32_t TwsData::request_candles(const std::string& symbol,
                                  const std::string& interval,
                                  const std::string& range) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mu_);
        reqs_.push_back({id, symbol, interval, range});
    }
    wake();
    return id;
}

uint32_t TwsData::subscribe_quotes(const std::vector<std::string>& symbols,
                                   int /*poll_s*/) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mu_);
        want_syms_ = symbols;
        want_dirty_ = true;
        quote_sub_ = id;
    }
    wake();
    return id;
}

void TwsData::unsubscribe(uint32_t sub_id) {
    std::lock_guard lock(mu_);
    if (sub_id != quote_sub_) return;   // superseded subscriptions are gone
    want_syms_.clear();
    want_dirty_ = true;
}

uint32_t TwsData::request_scan(const ScanSpec& spec, ScanCb cb) {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mu_);
        scan_req_ = {id, spec, std::move(cb)};
        scan_pending_ = true;
    }
    wake();
    return id;
}

// UI thread (/diag, /metrics, App::pump_preopen_gateway_check). GatewayAuth
// carries its own mutex; "now" is passed in so the policy stays testable.
bool TwsData::gateway_authed() const { return auth_.authed(steady_ms()); }
int64_t TwsData::gateway_auth_age_ms() const {
    return auth_.proof_age_ms(steady_ms());
}
int TwsData::gateway_farms_ok() const { return auth_.farms_ok(); }

std::string TwsData::account() const {
    std::lock_guard lock(mu_);
    return account_;
}

std::vector<std::string> TwsData::accounts() const {
    std::lock_guard lock(mu_);
    return accounts_;
}

void TwsData::log(std::string msg) {
    if (cbs_.on_log) cbs_.on_log("tws-data: " + std::move(msg));
}

void TwsData::wake() {
    if (auto* s = static_cast<EReaderOSSignal*>(wake_.load(std::memory_order_acquire)))
        s->issueSignal();
}

void TwsData::io_loop() {
    Io io(*this);
    wake_.store(&io.signal, std::memory_order_release);

    auto last_connect = std::chrono::steady_clock::time_point{};
    auto last_hist_reset = std::chrono::steady_clock::time_point{};
    int64_t last_nag_ms = 0;
    while (running_.load(std::memory_order_acquire)) {
        if (!io.client || !io.client->isConnected()) {
            io.drop_connection();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_connect >= std::chrono::seconds(3)) {
                last_connect = now;
                if (!io.connect_gateway()) {
                    const int64_t ms = now_ms();
                    if (ms - last_nag_ms > 30'000) {
                        last_nag_ms = ms;
                        log("cannot reach IB Gateway at " + host_ + ":" +
                            std::to_string(port_) +
                            " - run scripts\\Start-IbGateway.ps1");
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            io.signal.waitForSignal();
            if (io.reader) io.reader->processMsgs();
            const auto now = std::chrono::steady_clock::now();
            const int64_t now_steady = steady_ms();   // same clock, ms form
            // Half-open guard: socket up but no nextValidId within 10s.
            const bool stalled = !connected_.load(std::memory_order_acquire) &&
                                 now - last_connect > std::chrono::seconds(10);
            // Half-open *data* guard: connected, streaming fine, but a history
            // request has gone unanswered far past its normal turnaround.
            // A single unanswered request is retried in place first; only a
            // request that dies AGAIN after that retry means the session is
            // genuinely half-open and worth the (expensive) full teardown.
            if (connected_.load(std::memory_order_acquire)) {
                io.hist.prune_cancel_acks(now_steady);
                io.retry_dead_history(now_steady);
            }
            const bool hist_stalled = connected_.load(std::memory_order_acquire) &&
                                      io.hist.has_twice_dead(now_steady) &&
                                      now - last_hist_reset > kHistResetCooldown;
            if (io.reset_conn || stalled || hist_stalled) {
                if (io.reset_conn) {
                    // handshake-reject path already logged by the setter
                } else if (stalled) {
                    log("no API handshake within 10s - reconnecting");
                } else {
                    log("history request unanswered for >" +
                        std::to_string(kHistTimeoutMs / 1000) +
                        "s even after a retry - reconnecting data session "
                        "(half-open)");
                    last_hist_reset = now;
                }
                io.reset_conn = false;
                io.drop_connection();
                last_connect = std::chrono::steady_clock::now();   // full backoff
            }
            io.pump_requests();
        }
        // Publish history-fetch state for /diag (I/O thread owns io.hist).
        // These read a flat 0 before 0.15.0 because a retry was erased by its
        // own cancel ack the instant it was issued; a retried request now stays
        // in the pending set under its new id, aged from its FIRST attempt.
        //
        // That makes them honest, NOT a stall metric. An entry lives at most one
        // timeout plus one retry window (~40s, or ~100s while the escalation
        // sits in kHistResetCooldown) before it is answered, errored, or taken
        // out by drop_connection — so these can never show the 184-minute
        // 2026-08-10 outage, and a small number here is not evidence the history
        // path is healthy. Anything derived from in-flight requests is
        // structurally incapable of seeing that failure, which is exactly why
        // net/hist_freshness.h measures the SUCCESSES instead
        // (data.worst_last_bar_age_ms, which read 11,059,982 ms that day).
        pending_hist_.store(io.hist.pending(), std::memory_order_relaxed);
        oldest_hist_ms_.store(io.hist.oldest_age_ms(steady_ms()),
                              std::memory_order_relaxed);
    }

    wake_.store(nullptr, std::memory_order_release);
    io.drop_connection();
}

} // namespace tt::net
