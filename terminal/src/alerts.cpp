#include "alerts.h"

#include <windows.h>

#include <curl/curl.h>

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
        if (q_.size() >= 100) return;   // webhook down: don't hoard forever
        q_.push_back(text);
        ensure_worker();
    }
    cv_.notify_one();
}

void AlertNotifier::worker() {
    curl_global_init(CURL_GLOBAL_DEFAULT);   // idempotent, refcounted
    for (;;) {
        std::string text, url;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stop_.load() || !q_.empty(); });
            if (stop_.load() && q_.empty()) return;
            text = std::move(q_.front());
            q_.pop_front();
            url = webhook_;
        }
        if (url.empty()) continue;
        if (CURL* h = curl_easy_init()) {
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, text.c_str());
            curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink_cb);
            curl_easy_perform(h);   // best-effort: alerts must never wedge
            curl_easy_cleanup(h);
        }
    }
}

} // namespace tt::ui
