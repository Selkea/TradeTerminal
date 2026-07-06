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

inline int64_t net_steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct WsClient {
    CURL* ws = nullptr;
    std::string frame;   // fragment reassembly buffer

    ~WsClient() { close(); }

    bool open() const { return ws != nullptr; }

    void close() {
        if (ws) curl_easy_cleanup(ws);
        ws = nullptr;
        frame.clear();
    }

    // TLS connect only — protocol handshakes are the caller's business.
    // cookie: sent with the upgrade request (IBKR gateway wants its session
    // as "api=<token>"). insecure: skip peer verification — only ever for a
    // loopback gateway with a self-signed cert.
    bool connect(const std::string& url, const std::string& cookie = {},
                 bool insecure = false) {
        close();
        ws = curl_easy_init();
        if (!ws) return false;
        curl_easy_setopt(ws, CURLOPT_URL, url.c_str());
        curl_easy_setopt(ws, CURLOPT_CONNECT_ONLY, 2L);   // websocket, no callbacks
        // IBKR's backend 403s any request without a User-Agent.
        curl_easy_setopt(ws, CURLOPT_USERAGENT, "TradeTerminal/1.0");
        if (insecure) {
            curl_easy_setopt(ws, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(ws, CURLOPT_SSL_VERIFYHOST, 0L);
        } else {
            curl_easy_setopt(ws, CURLOPT_SSL_OPTIONS,
                             static_cast<long>(CURLSSLOPT_NATIVE_CA));
        }
        if (!cookie.empty()) curl_easy_setopt(ws, CURLOPT_COOKIE, cookie.c_str());
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
        const int64_t deadline = net_steady_ms() + timeout_ms;
        bool matched = false;
        while (!matched && net_steady_ms() < deadline &&
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

void net_ensure_curl_init();   // process-wide curl_global_init, idempotent

// Loopback UDP pair used as a select()-compatible wakeup pipe: the engine
// thread sends one byte so a broker I/O thread's select() wakes immediately
// for queued commands. Call net_ensure_curl_init() first (WSAStartup).
inline bool net_make_wake_pipe(uintptr_t& tx, uintptr_t& rx) {
    const SOCKET r = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (r == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (::bind(r, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::getsockname(r, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        ::closesocket(r);
        return false;
    }
    const SOCKET t = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (t == INVALID_SOCKET) {
        ::closesocket(r);
        return false;
    }
    ::connect(t, reinterpret_cast<sockaddr*>(&a), sizeof a);
    u_long nb = 1;
    ::ioctlsocket(r, FIONBIO, &nb);
    ::ioctlsocket(t, FIONBIO, &nb);
    tx = static_cast<uintptr_t>(t);
    rx = static_cast<uintptr_t>(r);
    return true;
}

} // namespace tt
