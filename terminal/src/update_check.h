#pragma once
// Periodically asks GitHub whether origin/main has moved past the commit this
// binary was built from. All networking happens on a private worker thread; the
// UI thread reads the small result via available()/remote_commit(). Best-effort:
// a failed poll leaves the previous state and retries on the next interval.

#include <atomic>
#include <cstdint>
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

    // slug e.g. "Selkea/TradeTerminal"; current_commit = the short commit this
    // binary was built from (TT_GIT_COMMIT); current_version = its semver
    // (TT_VERSION_BASE). When the commit is present we compare against
    // origin/main's HEAD; when it's missing/"unknown" (a build with no git
    // info) we fall back to comparing the VERSION file, so a bad git stamp can't
    // disable the checker. No-op only if the slug is empty or we have neither a
    // usable commit nor a version.
    void start(std::string repo_slug, std::string current_commit,
               std::string current_version);

    // origin/main is a different commit than the running binary.
    bool available() const { return available_.load(std::memory_order_acquire); }
    // Short SHA (current-commit length) of origin/main; "" until first success.
    std::string remote_commit() const;
    // Human semver of origin/main, read from its VERSION file — the "Latest"
    // shown in the update panel. Fetched only when an update is found; "" until
    // then (or if the fetch failed).
    std::string remote_version() const;
    // The commit this binary was built from (fixed after start()).
    const std::string& current_commit() const { return current_; }
    // Poke the worker to poll now instead of waiting for the next interval.
    void check_now() { poke_.store(true, std::memory_order_release); }
    // Bumps once per completed poll attempt — lets the UI tell when a poked
    // check has finished (compare against a snapshot taken at check_now()).
    uint32_t check_count() const { return checks_.load(std::memory_order_acquire); }
    // True if the most recent poll actually reached GitHub (vs a network error).
    bool last_ok() const { return last_ok_.load(std::memory_order_acquire); }

private:
    void worker();

    std::string slug_;
    std::string current_;          // build commit; set before thread starts, read-only
    std::string current_version_;  // build semver, for the no-commit fallback path
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> poke_{false};
    std::atomic<bool> available_{false};
    std::atomic<uint32_t> checks_{0};   // completed poll attempts
    std::atomic<bool> last_ok_{false};  // last poll reached GitHub
    mutable std::mutex mu_;
    std::string remote_;          // guarded by mu_
    std::string remote_version_;  // guarded by mu_ (origin/main's VERSION file)
};

} // namespace tt::ui
