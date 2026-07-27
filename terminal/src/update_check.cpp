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

// origin/main's semantic version, straight from the repo's VERSION file on the
// raw CDN. Returns "" on any error or if the payload doesn't look like a version.
std::string fetch_remote_version(const std::string& slug) {
    CURL* h = curl_easy_init();
    if (!h) return {};
    const std::string url =
        "https://raw.githubusercontent.com/" + slug + "/main/VERSION";
    std::string body;
    curl_slist* hdr = nullptr;
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
    const size_t b = body.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = body.find_last_not_of(" \t\r\n");
    body = body.substr(b, e - b + 1);
    // Sanity: short and starts with a digit (e.g. "0.1.1"); reject error pages.
    if (body.empty() || body.size() > 32 ||
        !std::isdigit(static_cast<unsigned char>(body[0])))
        return {};
    return body;
}
} // namespace

void UpdateChecker::start(std::string slug, std::string current,
                          std::string version) {
    slug_ = std::move(slug);
    current_ = std::move(current);
    current_version_ = std::move(version);
    // Need the repo, plus at least one way to tell "are we behind": the build
    // commit, or (when git info is missing) the semver from the VERSION file.
    const bool have_commit = !current_.empty() && current_ != "unknown";
    if (slug_.empty() || (!have_commit && current_version_.empty())) return;
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

std::string UpdateChecker::remote_version() const {
    std::lock_guard lock(mu_);
    return remote_version_;
}

void UpdateChecker::worker() {
    curl_global_init(CURL_GLOBAL_DEFAULT);   // idempotent, refcounted
    using namespace std::chrono;
    // First poll ~15s in (let the app settle / the network come up), then hourly
    // is plenty for a "someone pushed to main" nudge without hammering the API.
    auto next = steady_clock::now() + seconds(15);
    while (!stop_.load(std::memory_order_acquire)) {
        if (poke_.exchange(false) || steady_clock::now() >= next) {
            const bool have_commit = !current_.empty() && current_ != "unknown";
            const std::string sha = fetch_head_sha(slug_);
            // origin/main's short SHA for the panel (independent of our commit).
            const std::string shortsha =
                sha.empty() ? std::string()
                            : sha.substr(0, have_commit ? current_.size() : 12);
            bool ok = false, avail = false;
            std::string ver;
            if (have_commit) {
                if (!sha.empty()) {
                    ok = true;
                    avail = shortsha != current_;
                    // Remote VERSION only matters when behind — fetch it in the
                    // same poll so the panel has "Latest" the moment it appears.
                    if (avail) ver = fetch_remote_version(slug_);
                }
            } else {
                // No usable build commit: fall back to comparing the VERSION
                // file, so a build with no git info can still see updates.
                const std::string rv = fetch_remote_version(slug_);
                if (!rv.empty()) {
                    ok = true;
                    avail = rv != current_version_;
                    if (avail) ver = rv;
                }
            }
            if (ok) {
                {
                    std::lock_guard lock(mu_);
                    if (!shortsha.empty()) remote_ = shortsha;
                    remote_version_ = ver;   // "" when up to date or fetch failed
                }
                available_.store(avail, std::memory_order_release);
            }
            last_ok_.store(ok, std::memory_order_release);
            checks_.fetch_add(1, std::memory_order_release);   // signal completion
            next = steady_clock::now() + minutes(60);
        }
        std::this_thread::sleep_for(seconds(1));   // stays responsive to stop_/poke_
    }
}

} // namespace tt::ui
