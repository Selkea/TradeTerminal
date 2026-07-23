#include "update_check.h"

#include <cctype>
#include <chrono>

#include <curl/curl.h>

namespace tt::ui {

namespace {
size_t write_cb(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

// GitHub's ".sha" media type makes /commits/<ref> answer with the bare 40-char
// commit SHA as plain text (no JSON) — the whole point of the check in one line.
// Returns "" on any transport/HTTP error (best-effort: caller just retries).
std::string fetch_head_sha(const std::string& slug) {
    CURL* h = curl_easy_init();
    if (!h) return {};
    const std::string url =
        "https://api.github.com/repos/" + slug + "/commits/main";
    std::string body;
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Accept: application/vnd.github.sha");
    hdr = curl_slist_append(hdr, "User-Agent: TradeTerminal-update-check");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK || code != 200) return {};
    while (!body.empty() &&
           (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
        body.pop_back();
    // Sanity: a commit SHA is hex and at least our short-length. Anything else
    // (an error page, a redirect body) is treated as "no answer".
    if (body.size() < 12) return {};
    for (const char c : body)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    return body;
}
} // namespace

void UpdateChecker::start(std::string slug, std::string current) {
    slug_ = std::move(slug);
    current_ = std::move(current);
    if (slug_.empty() || current_.empty() || current_ == "unknown") return;
    th_ = std::thread([this] { worker(); });
}

UpdateChecker::~UpdateChecker() {
    stop_.store(true, std::memory_order_release);
    if (th_.joinable()) th_.join();
}

std::string UpdateChecker::remote_commit() const {
    std::lock_guard lock(mu_);
    return remote_;
}

void UpdateChecker::worker() {
    curl_global_init(CURL_GLOBAL_DEFAULT);   // idempotent, refcounted
    using namespace std::chrono;
    // First poll ~15s in (let the app settle / the network come up), then hourly
    // is plenty for a "someone pushed to main" nudge without hammering the API.
    auto next = steady_clock::now() + seconds(15);
    while (!stop_.load(std::memory_order_acquire)) {
        if (poke_.exchange(false) || steady_clock::now() >= next) {
            const std::string sha = fetch_head_sha(slug_);
            if (!sha.empty()) {
                const std::string shortsha = sha.substr(0, current_.size());
                {
                    std::lock_guard lock(mu_);
                    remote_ = shortsha;
                }
                available_.store(shortsha != current_, std::memory_order_release);
            }
            next = steady_clock::now() + minutes(60);
        }
        std::this_thread::sleep_for(seconds(1));   // stays responsive to stop_/poke_
    }
}

} // namespace tt::ui
