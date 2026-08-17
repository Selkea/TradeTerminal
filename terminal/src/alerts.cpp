#include "alerts.h"

#include <windows.h>

#include <curl/curl.h>

#include <chrono>

namespace tt::ui {

namespace {
size_t sink_cb(char*, size_t sz, size_t nm, void*) { return sz * nm; }
} // namespace

AlertNotifier::~AlertNotifier() {
    stop_.store(true, std::memory_order_release);
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

void AlertNotifier::ensure_worker() {
    if (!th_.joinable()) th_ = std::thread([this] { worker(); });
}

void AlertNotifier::notify(Severity sev, const std::string& text) {
    if (muted()) return;
    if (sev == Critical) MessageBeep(MB_ICONHAND);
    else if (sev == Warning) MessageBeep(MB_ICONEXCLAMATION);
    {
        std::lock_guard lock(mu_);
        if (webhook_.empty()) return;
        if (q_.size() >= 100) {   // webhook down: don't hoard forever
            ++del_.dropped;
            return;
        }
        q_.push_back({sev, text});
        ensure_worker();
    }
    cv_.notify_one();
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
            cv_.wait(lock, [this] { return stop_.load() || !q_.empty(); });
            if (stop_.load() && q_.empty()) return;
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
