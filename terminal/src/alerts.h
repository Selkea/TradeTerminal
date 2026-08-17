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
#include <deque>
#include <mutex>
#include <string>
#include <thread>

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
} // namespace detail

class AlertNotifier {
public:
    enum Severity { Info = 0, Warning = 1, Critical = 2 };

    AlertNotifier() = default;
    ~AlertNotifier();

    void set_webhook(std::string url);   // empty = beeps only
    bool has_webhook() const;
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

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

private:
    void worker();
    void ensure_worker();
    // One POST. Returns true on 2xx; fills status/err either way.
    bool post_once(const std::string& url, const std::string& body, long& status,
                   std::string& err);

    struct Item {
        Severity sev;
        std::string text;
    };

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    std::string webhook_;
    Delivery del_;
    std::atomic<int> retry_backoff_ms_{3000};
    std::atomic<bool> muted_{false};
    std::atomic<bool> stop_{false};
    std::thread th_;
};

} // namespace tt::ui
