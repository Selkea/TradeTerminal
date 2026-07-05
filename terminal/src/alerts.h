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
