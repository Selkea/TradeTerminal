#pragma once
// Internal to tt_alpaca: thin RAII wrapper over libcurl's websocket API,
// shared by the trading stream (alpaca_broker.cpp) and the market-data
// stream (alpaca_feed.cpp). One owning thread per instance; not thread-safe.

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace tt {

inline int64_t alpaca_steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct AlpacaWs {
    CURL* ws = nullptr;
    std::string frame;   // fragment reassembly buffer

    ~AlpacaWs() { close(); }

    bool open() const { return ws != nullptr; }

    void close() {
        if (ws) curl_easy_cleanup(ws);
        ws = nullptr;
        frame.clear();
    }

    // TLS connect only — protocol handshakes are the caller's business.
    bool connect(const std::string& url) {
        close();
        ws = curl_easy_init();
        if (!ws) return false;
        curl_easy_setopt(ws, CURLOPT_URL, url.c_str());
        curl_easy_setopt(ws, CURLOPT_CONNECT_ONLY, 2L);   // websocket, no callbacks
        curl_easy_setopt(ws, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
        curl_easy_setopt(ws, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        if (curl_easy_perform(ws) != CURLE_OK) {
            close();
            return false;
        }
        return true;
    }

    bool send_text(const std::string& msg) {
        size_t off = 0;
        while (off < msg.size()) {
            size_t sent = 0;
            const CURLcode rc =
                curl_ws_send(ws, msg.data() + off, msg.size() - off, &sent, 0, CURLWS_TEXT);
            if (rc == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rc != CURLE_OK) return false;
            off += sent;
        }
        return true;
    }

    // Blocks until the socket is readable, the timeout passes, or the
    // connection is unusable. Lets I/O threads wake the instant bytes
    // arrive instead of sleeping on a timer (which adds up to the full
    // sleep to every tick/fill that lands mid-sleep).
    bool wait_readable(int timeout_ms) {
        if (!ws) return false;
        curl_socket_t s = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(ws, CURLINFO_ACTIVESOCKET, &s) != CURLE_OK ||
            s == CURL_SOCKET_BAD)
            return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        return select(0, &rd, nullptr, nullptr, &tv) > 0;
    }

    // Drives recv; calls on_msg(std::string_view) for each complete message.
    // Returns 0 = idle, 1 = progressed, -1 = connection lost.
    template <typename Fn>
    int poll(Fn&& on_msg) {
        char buf[4096];
        int progressed = 0;
        for (;;) {
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode rc = curl_ws_recv(ws, buf, sizeof buf, &n, &meta);
            if (rc == CURLE_AGAIN) return progressed;
            if (rc != CURLE_OK || !meta) return -1;
            if (meta->flags & CURLWS_CLOSE) return -1;
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
            frame.append(buf, n);
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                on_msg(std::string_view(frame));
                frame.clear();
            }
            progressed = 1;
        }
    }

    // Poll until pred(msg) returns true, the deadline passes, or *stop.
    // False also when the connection drops.
    template <typename Pred>
    bool wait(int timeout_ms, const std::atomic<bool>& stop, Pred&& pred) {
        const int64_t deadline = alpaca_steady_ms() + timeout_ms;
        bool matched = false;
        while (!matched && alpaca_steady_ms() < deadline &&
               !stop.load(std::memory_order_relaxed)) {
            const int r = poll([&](std::string_view msg) {
                if (pred(msg)) matched = true;
            });
            if (r < 0) return false;
            if (r == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return matched;
    }
};

void alpaca_ensure_curl_init();   // process-wide curl_global_init, idempotent

} // namespace tt
