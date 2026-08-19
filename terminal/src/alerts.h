#pragma once
// Alerts: the risk halts and fills the engine logs are only useful if you
// hear about them away from the desk. UI thread calls notify(); a worker
// thread does the beep-adjacent webhook POST so nothing render- or
// engine-adjacent ever waits on the network.
//
// Webhook body is plain text — works as-is with ntfy.sh topics and most
// generic webhook receivers. Configure via config.json "alert_webhook" or
// the TT_ALERT_WEBHOOK env var (env wins).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace tt::ui {

namespace detail {
// ntfy.sh (and any header-based push receiver) serves a request body containing
// non-ASCII bytes as a downloadable "attachment.txt" instead of an inline
// message — so an alert whose text has an em-dash (many of ours do, e.g.
// "RISK HALT (...) — broker cancel-all + flatten...") would reach the phone
// unreadable. Fold the webhook payload to ASCII: collapse each run of non-ASCII
// bytes to a single '-'. Only the outbound payload is folded; console/log text
// keeps its em-dashes.
inline std::string ascii_fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_nonascii = false;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            prev_nonascii = false;
        } else if (!prev_nonascii) {
            out.push_back('-');
            prev_nonascii = true;
        }
    }
    return out;
}

// THE COALESCING KEY. A flood is the SAME event repeating, and the same event
// almost never repeats byte-for-byte: it carries an order id, a share count, a
// price, a symbol's last trade. Exact-text matching would therefore have missed
// the very burst that motivated this — "error 201 (id 5): Order rejected" and
// "error 201 (id 6): Order rejected" are one event, twice.
//
// So the key is the text with every NUMBER folded to '#'. A run of digits, plus
// any '.' or ',' sitting *between* digits, collapses to a single '#'; a '.' that
// ends a sentence is left alone, because "closed." and "closed" must not become
// different keys.
//
// This deliberately merges two lines that differ only in their numbers — which
// is the entire point, and is safe because merging never DISCARDS: the first of
// a burst always goes out, and everything swallowed after it is counted and
// reported in the summary that closes the window.
inline std::string burst_key(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i < s.size();) {
        if (!is_digit(s[i])) {
            out.push_back(s[i++]);
            continue;
        }
        while (i < s.size() &&
               (is_digit(s[i]) ||
                ((s[i] == '.' || s[i] == ',') && i + 1 < s.size() && is_digit(s[i + 1]))))
            ++i;
        out.push_back('#');
    }
    return out;
}
} // namespace detail

class AlertNotifier {
public:
    enum Severity { Info = 0, Warning = 1, Critical = 2 };

    // THE RATE CAP, per severity and deliberately NOT shared. A cap on total
    // pages would let a flood of Infos eat the budget a Critical needs — the
    // same starvation the queue's severity-aware eviction exists to prevent,
    // one layer up. Each tier refills at its own rate from its own bucket, so
    // the loudest tier is reachable no matter what the quiet ones are doing.
    //
    // These are backstops, not the primary defence: coalescing already folds a
    // repeating event into one page, so reaching a cap means a flood of
    // genuinely DISTINCT alerts, which is itself the emergency.
    static constexpr double kRatePerMin[3] = {6.0, 12.0, 30.0};

    AlertNotifier() = default;
    ~AlertNotifier();

    void set_webhook(std::string url);   // empty = beeps only
    bool has_webhook() const;
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

    // TEST SEAM, process-wide and deliberately not per-instance. MessageBeep is
    // a REAL system sound on the developer's desktop, and the flood tests below
    // raise alerts by the dozen — running the suite (let alone a mutation
    // sweep, which runs it once per mutant) turned the machine into an alarm
    // clock. Nothing about the beep is under test; the tiering that decides
    // whether one happens is, and that is observable without making noise.
    static void set_beeps_enabled(bool e) { s_beeps_.store(e, std::memory_order_relaxed); }

    // UI thread. Info: webhook only. Warning/Critical: system beep + webhook.
    void notify(Severity sev, const std::string& text);

    // DID THE PAGE ACTUALLY ARRIVE? Until 0.29.2 nothing here could answer that:
    // the worker called curl_easy_perform and discarded the result, so a 429, a
    // 500 and a DNS failure were all indistinguishable from delivery.
    //
    // That is not hypothetical. On 2026-08-17 an optimizer bug sent 73 junk
    // pages in 90 seconds; the two CRITICAL pages that followed two minutes
    // later — an off-lineup broker position and a confirmed book divergence,
    // the loudest two things this app can say — never reached the phone. The
    // app logged both as sent and had no idea. The flood is fixed (0.29.1), but
    // a Critical page can fail for reasons that have nothing to do with us, and
    // "the operator was never told" must be a visible state rather than silence.
    struct Delivery {
        uint64_t sent = 0;        // 2xx responses
        uint64_t failed = 0;      // gave up after the retries below
        uint64_t retried = 0;     // attempts beyond the first
        uint64_t dropped = 0;     // never even queued (backlog full)
        // A LOWER-TIER alert removed to make room for a higher one. Distinct
        // from `dropped`: that is "we refused the new page", this is "we made
        // room for it". Both are losses and both must be visible, but only the
        // second means the severity rule did its job.
        uint64_t evicted = 0;
        // Folded into a burst already in flight (same key, inside the window).
        uint64_t coalesced = 0;
        // Refused by the per-severity rate cap above.
        uint64_t throttled = 0;
        // Burst-summary pages emitted. Every coalesced/throttled alert is
        // accounted for by exactly one of these, which is what keeps
        // suppression from becoming the silence it replaced.
        uint64_t summaries = 0;
        // Discarded because the operator turned the channel OFF (Alerts menu).
        // A muted notifier threw away Criticals and left no trace of having done
        // so, which made "alerts are muted" indistinguishable from "nothing
        // happened" — the exact confusion 0.29.2 was written to end, reachable
        // from a menu item. /diag reports the flag and this count together.
        uint64_t muted_discarded = 0;
        long last_status = 0;     // last HTTP status seen (0 = transport error)
        std::string last_error;   // curl's message, or "HTTP <code>"
    };
    Delivery delivery() const;

    // TEST SEAM. Production waits whole seconds because that is the timescale a
    // rate-limit bucket refills on; a suite cannot afford to. Only the delay
    // changes — the attempt counts and the success rule are the shipping ones.
    void set_retry_backoff_ms(int ms) {
        retry_backoff_ms_.store(ms, std::memory_order_relaxed);
    }

    // TEST SEAM. Holds the worker so the QUEUE ADMISSION POLICY can be tested
    // deterministically. The policy is the unit under test; the drain is not,
    // and a test that races them cannot say whether a full queue evicted or
    // simply had room. Never set in production.
    void set_worker_paused(bool p) {
        { std::lock_guard lock(mu_); paused_ = p; }
        cv_.notify_all();
    }

    // TEST SEAM. The burst window is a MINUTE in production — a suite that
    // waited it out would take longer than every other test combined, and one
    // that slept even a shortened version would reintroduce the 233 s
    // regression 0.29.2 just paid to remove. Injecting the clock lets window
    // expiry, summary emission and bucket refill all be tested at zero cost and
    // with no timing race. Defaults to steady_clock.
    void set_clock_for_test(std::function<int64_t()> f);
    void set_burst_window_ms(int ms) {
        burst_window_ms_.store(ms, std::memory_order_relaxed);
    }
    // How often the worker wakes to close expired windows. Also a seam so the
    // one test that proves the worker really does the flushing (rather than
    // calling the flush directly) costs milliseconds instead of a second.
    void set_flush_interval_ms(int ms) {
        flush_interval_ms_.store(ms, std::memory_order_relaxed);
    }

private:
    void worker();
    void ensure_worker();
    // One POST. Returns true on 2xx; fills status/err either way.
    bool post_once(const std::string& url, const std::string& body, long& status,
                   std::string& err);
    int64_t now_ms() const;
    // Push onto the backlog, applying the severity-aware cap below. Caller
    // holds mu_. Returns true if it made it in. Both a fresh alert and a burst
    // summary go through here, so neither can bypass the cap.
    bool enqueue(Severity sev, std::string text);
    // Close every burst window that has expired, queueing a summary for any
    // that swallowed something. Caller holds mu_. Returns true if it queued.
    bool flush_bursts(int64_t now);
    // Consume one token of `sev`'s bucket. Caller holds mu_.
    bool take_token(Severity sev, int64_t now);

    struct Item {
        Severity sev;
        std::string text;
    };

    // One in-flight burst: a key that has paged recently and is now swallowing
    // its own repeats until the window closes.
    struct Burst {
        int64_t last_ms = 0;      // when this key last PAGED (or was first seen)
        uint64_t suppressed = 0;  // coalesced + throttled since then
        Severity sev = Info;
        std::string sample;       // most recent suppressed text, for the summary
    };

    // A flood of DISTINCT keys must not grow this without bound. Past the cap
    // new keys simply go untracked — they are still rate-capped, which is the
    // backstop that actually bounds the channel.
    static constexpr size_t kMaxBursts = 512;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    std::string webhook_;
    Delivery del_;
    std::unordered_map<std::string, Burst> bursts_;
    double tokens_[3] = {kRatePerMin[0], kRatePerMin[1], kRatePerMin[2]};
    int64_t tokens_stamp_ms_ = 0;
    // RATE-CAPPED ALERTS ARE ACCUMULATED PER SEVERITY, NOT PER LINE. The first
    // cut of this gave each throttled alert its own burst entry, so 194 refused
    // Infos produced 194 summary pages the instant the window closed — and
    // summaries bypass the cap on purpose. The defence would have been a worse
    // flood than the one it exists to stop. One report per tier per window
    // instead, which is what makes the cap's own output bounded.
    uint64_t throttled_pending_[3] = {0, 0, 0};
    int64_t throttled_since_ms_[3] = {0, 0, 0};
    std::string throttled_sample_[3];
    static inline std::atomic<bool> s_beeps_{true};
    std::function<int64_t()> clock_;
    bool paused_ = false;
    std::atomic<int> retry_backoff_ms_{3000};
    std::atomic<int> burst_window_ms_{60000};
    std::atomic<int> flush_interval_ms_{1000};
    std::atomic<bool> muted_{false};
    std::atomic<bool> stop_{false};
    std::thread th_;
};

} // namespace tt::ui
