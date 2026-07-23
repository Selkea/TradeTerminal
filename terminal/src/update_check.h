#pragma once
// Periodically asks GitHub whether origin/main has moved past the commit this
// binary was built from. All networking happens on a private worker thread; the
// UI thread reads the small result via available()/remote_commit(). Best-effort:
// a failed poll leaves the previous state and retries on the next interval.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace tt::ui {

class UpdateChecker {
public:
    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // slug e.g. "Selkea/TradeTerminal"; current = the short commit this binary
    // was built from (TT_GIT_COMMIT). No-op (never flags an update) if either is
    // empty or current is "unknown" (a build with no git info).
    void start(std::string repo_slug, std::string current_commit);

    // origin/main is a different commit than the running binary.
    bool available() const { return available_.load(std::memory_order_acquire); }
    // Short SHA (current-commit length) of origin/main; "" until first success.
    std::string remote_commit() const;
    // The commit this binary was built from (fixed after start()).
    const std::string& current_commit() const { return current_; }
    // Poke the worker to poll now instead of waiting for the next interval.
    void check_now() { poke_.store(true, std::memory_order_release); }

private:
    void worker();

    std::string slug_;
    std::string current_;   // set before the thread starts; read-only after
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> poke_{false};
    std::atomic<bool> available_{false};
    mutable std::mutex mu_;
    std::string remote_;    // guarded by mu_
};

} // namespace tt::ui
