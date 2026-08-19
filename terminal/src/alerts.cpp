#include "alerts.h"

#include <windows.h>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>

namespace tt::ui {

namespace {
size_t sink_cb(char*, size_t sz, size_t nm, void*) { return sz * nm; }
} // namespace

AlertNotifier::~AlertNotifier() {
    // UNDER THE MUTEX. The worker evaluates stop_ inside a predicate held on
    // mu_, and the lock is held continuously from that load returning false
    // into cv_.wait's atomic unlock-and-block. A destructor that never
    // contends for mu_ can run entirely inside that window, find no waiter
    // registered, and lose the notification — leaving join() below blocked
    // forever on a thread waiting for a signal that already happened.
    {
        std::lock_guard lock(mu_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void AlertNotifier::set_webhook(std::string url) {
    std::lock_guard lock(mu_);
    webhook_ = std::move(url);
}

bool AlertNotifier::has_webhook() const {
    std::lock_guard lock(mu_);
    return !webhook_.empty();
}

void AlertNotifier::set_clock_for_test(std::function<int64_t()> f) {
    std::lock_guard lock(mu_);
    clock_ = std::move(f);
}

// ALWAYS called with mu_ held — that is what makes clock_ safe to read without
// its own synchronisation.
int64_t AlertNotifier::now_ms() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AlertNotifier::ensure_worker() {
    if (!th_.joinable()) th_ = std::thread([this] { worker(); });
}

bool AlertNotifier::enqueue(Severity sev, std::string text) {
    // THE CAP IS SEVERITY-AWARE. It used to drop whatever arrived once the
    // queue held 100, consulting only the size — so a Critical raised during
    // an Info backlog was discarded while the stale Infos ahead of it were
    // kept. That is precisely backwards, and it is the in-process twin of the
    // 2026-08-17 loss: there the two Critical pages died at the receiver, here
    // they would die before a socket was ever opened.
    //
    // A higher tier now evicts the OLDEST strictly-lower-tier item instead of
    // being refused. Equal-or-higher tiers are never evicted, so a flood of
    // Criticals still bounds the queue rather than cannibalising itself, and
    // an Info arriving into a full queue is dropped exactly as before.
    if (q_.size() >= 100) {
        auto victim = q_.end();
        for (auto it = q_.begin(); it != q_.end(); ++it)
            if (it->sev < sev) { victim = it; break; }   // oldest lower tier
        if (victim == q_.end()) {
            ++del_.dropped;      // nothing outranked: this one goes
            return false;
        }
        q_.erase(victim);
        ++del_.evicted;          // the evicted item is still a loss; count it
    }
    q_.push_back({sev, std::move(text)});
    return true;
}

bool AlertNotifier::take_token(Severity sev, int64_t now) {
    if (tokens_stamp_ms_ == 0) tokens_stamp_ms_ = now;
    const int64_t dt = now - tokens_stamp_ms_;
    if (dt > 0) {
        for (int i = 0; i < 3; ++i)
            tokens_[i] = std::min(kRatePerMin[i], tokens_[i] + dt * kRatePerMin[i] / 60000.0);
        tokens_stamp_ms_ = now;
    }
    const int i = static_cast<int>(sev);
    if (tokens_[i] < 1.0) return false;
    tokens_[i] -= 1.0;
    return true;
}

bool AlertNotifier::flush_bursts(int64_t now) {
    const int64_t win = burst_window_ms_.load(std::memory_order_relaxed);
    bool queued = false;
    for (auto it = bursts_.begin(); it != bursts_.end();) {
        if (now - it->second.last_ms < win) { ++it; continue; }
        const Burst& b = it->second;
        if (b.suppressed > 0 && !webhook_.empty()) {
            // THE SUMMARY BYPASSES THE RATE CAP, on purpose. It is at most one
            // page per key per window, and it is the only record that anything
            // was suppressed at all — capping the report of a flood along with
            // the flood is how suppression turns back into the silence this
            // whole layer exists to remove.
            std::string s = b.sample + "  [+" + std::to_string(b.suppressed) +
                            " more like this in the last " +
                            std::to_string((now - b.last_ms) / 1000) + "s]";
            if (enqueue(b.sev, std::move(s))) {
                ++del_.summaries;
                queued = true;
            }
        }
        it = bursts_.erase(it);
    }
    // ONE report per tier for everything the rate cap refused. Bounded by
    // construction, unlike a summary per refused line — see the members' note.
    for (int s = 0; s < 3; ++s) {
        if (throttled_pending_[s] == 0) continue;
        if (now - throttled_since_ms_[s] < win) continue;
        if (!webhook_.empty()) {
            std::string t = "ALERT RATE CAP: " + std::to_string(throttled_pending_[s]) +
                            " further alerts suppressed in the last " +
                            std::to_string((now - throttled_since_ms_[s]) / 1000) +
                            "s (most recent: " + throttled_sample_[s] + ")";
            if (enqueue(static_cast<Severity>(s), std::move(t))) {
                ++del_.summaries;
                queued = true;
            }
        }
        throttled_pending_[s] = 0;
        throttled_sample_[s].clear();
    }
    return queued;
}

void AlertNotifier::notify(Severity sev, const std::string& text) {
    if (muted()) return;
    bool admit = false;
    bool wake = false;
    {
        std::lock_guard lock(mu_);
        const int64_t now = now_ms();
        wake = flush_bursts(now);

        // COALESCE FIRST, then rate-cap. The order matters: a repeating event
        // must not spend tokens the rate cap is holding for something new, and
        // on 2026-08-17 the flood was 73 copies of ONE line. Keying on the text
        // (numbers folded — see detail::burst_key) means the first copy pages
        // immediately and at full speed, which is the property that keeps this
        // from delaying a real alert: suppression only ever applies to the
        // second and later copies of something already sent.
        const std::string key =
            std::to_string(static_cast<int>(sev)) + "\x1f" + detail::burst_key(text);
        auto it = bursts_.find(key);
        if (it != bursts_.end()) {
            ++it->second.suppressed;
            it->second.sample = text;
            ++del_.coalesced;
        } else if (!take_token(sev, now)) {
            // A flood of DISTINCT alerts — coalescing cannot help, so the cap
            // does. Accumulated per severity rather than per line so the report
            // stays one page; a throttled alert that vanished uncounted would
            // be the 0.29.2 bug rebuilt one layer higher.
            ++del_.throttled;
            const int s = static_cast<int>(sev);
            if (throttled_pending_[s] == 0) throttled_since_ms_[s] = now;
            ++throttled_pending_[s];
            throttled_sample_[s] = text;
        } else {
            admit = true;
            if (bursts_.size() < kMaxBursts)
                bursts_.emplace(key, Burst{now, 0, sev, text});
        }

        if (!webhook_.empty()) {
            if (admit && enqueue(sev, text)) wake = true;
            ensure_worker();   // also the thing that closes burst windows
        }
    }
    if (wake) cv_.notify_one();
    // BEEP ONLY FOR AN ADMITTED ALERT, and outside the lock. A burst used to
    // beep once per copy: 73 in 90 seconds, which is not an alarm, it is a
    // reason to leave the room.
    if (admit && s_beeps_.load(std::memory_order_relaxed)) {
        if (sev == Critical) MessageBeep(MB_ICONHAND);
        else if (sev == Warning) MessageBeep(MB_ICONEXCLAMATION);
    }
}

AlertNotifier::Delivery AlertNotifier::delivery() const {
    std::lock_guard lock(mu_);
    return del_;
}

bool AlertNotifier::post_once(const std::string& url, const std::string& body,
                              long& status, std::string& err) {
    status = 0;
    err.clear();
    CURL* h = curl_easy_init();
    if (!h) {
        err = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink_cb);
    const CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (status >= 200 && status < 300) return true;
    err = "HTTP " + std::to_string(status);
    return false;
}

void AlertNotifier::worker() {
    curl_global_init(CURL_GLOBAL_DEFAULT);   // idempotent, refcounted
    for (;;) {
        Item item;
        std::string url;
        {
            std::unique_lock lock(mu_);
            // WAIT WITH A TIMEOUT so burst windows close on their own. A
            // summary that only appeared when the NEXT alert happened to
            // arrive would never appear at all in the case that matters most:
            // a flood that stops, leaving the operator with the one page that
            // opened it and no idea 72 more followed.
            cv_.wait_for(lock,
                         std::chrono::milliseconds(
                             flush_interval_ms_.load(std::memory_order_relaxed)),
                         [this] { return stop_.load() || (!q_.empty() && !paused_); });
            if (stop_.load()) {
                // Drain regardless of paused_ — honouring the test pause here
                // would spin this loop forever and hang the destructor's join.
                if (q_.empty()) return;
            } else if (q_.empty() || paused_) {
                if (flush_bursts(now_ms())) continue;   // re-loop to send it
                continue;
            }
            item = std::move(q_.front());
            q_.pop_front();
            url = webhook_;
        }
        if (url.empty()) continue;
        const std::string body = detail::ascii_fold(item.text);   // inline, not attachment

        // RETRY, and only for the tiers worth waking someone for. The failure
        // that motivated this is a receiver-side RATE LIMIT: ntfy.sh replenishes
        // a depleted bucket over seconds, so a page dropped at T is very likely
        // to be accepted a few seconds later — which is exactly the window a
        // best-effort single shot threw away. Info is not retried: it is the
        // tier for things nobody is woken for, and retrying a flood of them is
        // how a rate limit gets held open.
        const int attempts = item.sev == Info ? 1 : 3;
        long status = 0;
        std::string err;
        bool ok = false;
        for (int i = 0; i < attempts && !ok; ++i) {
            if (i > 0) {
                // Backoff past a depleted bucket (ntfy replenishes ~1/5 s),
                // interruptible so shutdown never waits on a retry.
                std::unique_lock lock(mu_);
                cv_.wait_for(lock,
                             std::chrono::milliseconds(
                                 retry_backoff_ms_.load(std::memory_order_relaxed) * i),
                             [this] { return stop_.load(); });
                if (stop_.load()) break;
                ++del_.retried;
            }
            ok = post_once(url, body, status, err);
        }
        {
            std::lock_guard lock(mu_);
            del_.last_status = status;
            if (ok) {
                ++del_.sent;
                del_.last_error.clear();
            } else {
                ++del_.failed;
                del_.last_error = err;
            }
        }
    }
}

} // namespace tt::ui
