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

private:
    void worker();
    void ensure_worker();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    std::string webhook_;
    std::atomic<bool> muted_{false};
    std::atomic<bool> stop_{false};
    std::thread th_;
};

} // namespace tt::ui
