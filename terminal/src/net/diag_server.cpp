#include "net/diag_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace tt::ui {
namespace {

// Read request headers (GET has no body we act on). Stops at the blank-line
// terminator, on disconnect/timeout, or at a size cap that guards against a
// client that never terminates its headers. Returns false only if nothing at
// all arrived.
bool read_request(SOCKET s, std::string& out) {
    char buf[2048];
    out.clear();
    for (;;) {
        const int n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) return !out.empty();          // timeout/close: use what we have
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) return true;
        if (out.size() > 16 * 1024) return true;  // header-flood guard
    }
}

std::string http_response(int code, const char* status, std::string_view ctype,
                          std::string_view body) {
    std::string r;
    r.reserve(body.size() + 160);
    r += "HTTP/1.1 ";
    r += std::to_string(code);
    r += ' ';
    r += status;
    r += "\r\nContent-Type: ";
    r += ctype;
    r += "\r\nContent-Length: ";
    r += std::to_string(body.size());
    r += "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
    r += body;
    return r;
}

// Returns false if the peer went away mid-send (used to end an SSE stream).
bool send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(s, data.data() + off,
                             static_cast<int>(data.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string to_lower(std::string_view v) {
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "GET /path?query HTTP/1.1" -> method, target (path+query).
void parse_request_line(std::string_view req, std::string& method, std::string& target) {
    const size_t eol = req.find("\r\n");
    std::string_view line = req.substr(0, eol == std::string_view::npos ? req.size() : eol);
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return;
    method = std::string(line.substr(0, sp1));
    const size_t sp2 = line.find(' ', sp1 + 1);
    target = std::string(line.substr(sp1 + 1,
                                     sp2 == std::string_view::npos ? std::string_view::npos
                                                                   : sp2 - sp1 - 1));
}

// Value of the first "Authorization: Bearer <x>" header, or "".
std::string header_bearer(std::string_view req) {
    const std::string lower = to_lower(req);
    size_t pos = lower.find("\r\nauthorization:");
    if (pos == std::string::npos) return {};
    pos += 2;                                   // to the header name
    const size_t eol = req.find("\r\n", pos);
    std::string_view val = req.substr(pos, (eol == std::string_view::npos ? req.size() : eol) - pos);
    const size_t colon = val.find(':');
    if (colon == std::string_view::npos) return {};
    val = val.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
    const std::string_view kBearer = "Bearer ";
    if (val.size() < kBearer.size() || to_lower(val.substr(0, kBearer.size())) != "bearer ")
        return {};
    val = val.substr(kBearer.size());
    while (!val.empty() && val.back() == ' ') val.remove_suffix(1);
    return std::string(val);
}

// Value of the header named `lname` (already lowercase), trimmed, or "".
std::string header_value(std::string_view req, std::string_view lname) {
    const std::string lower = to_lower(req);
    std::string needle = "\r\n";
    needle += lname;
    needle += ':';
    const size_t pos = lower.find(needle);
    if (pos == std::string::npos) return {};
    const size_t vstart = pos + needle.size();
    const size_t eol = req.find("\r\n", vstart);
    std::string_view val =
        req.substr(vstart, (eol == std::string_view::npos ? req.size() : eol) - vstart);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
    return std::string(val);
}

// Value of ?<key>=<x> (or &<key>=) in the request target, or "".
std::string query_param(std::string_view target, std::string_view key) {
    const size_t q = target.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view qs = target.substr(q + 1);
    for (size_t i = 0; i < qs.size();) {
        const size_t amp = qs.find('&', i);
        std::string_view kv = qs.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == key)
            return std::string(kv.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return {};
}

} // namespace

DiagServer::~DiagServer() { stop(); }

bool DiagServer::start(const std::string& host, uint16_t port, std::string token,
                       BodyProvider diag, LogsProvider logs, BodyProvider root,
                       LogFn on_log) {
    if (running()) return true;
    token_ = std::move(token);
    diag_ = std::move(diag);
    logs_ = std::move(logs);
    root_ = std::move(root);
    log_ = std::move(on_log);
    stop_.store(false, std::memory_order_relaxed);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // refcounted; curl already did one

    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (log_) log_("diag: socket() failed");
        WSACleanup();
        return false;
    }
    BOOL yes = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (log_)
            log_("diag: bind " + host + ":" + std::to_string(port) + " failed (WSA " +
                 std::to_string(WSAGetLastError()) + ")");
        ::closesocket(s);
        WSACleanup();
        return false;
    }
    if (::listen(s, 8) != 0) {
        if (log_) log_("diag: listen failed");
        ::closesocket(s);
        WSACleanup();
        return false;
    }
    listen_sock_ = static_cast<uintptr_t>(s);
    thread_ = std::thread(&DiagServer::accept_loop, this);
    return true;
}

void DiagServer::set_control(std::string token, ControlFn fn) {
    control_token_ = std::move(token);
    control_ = std::move(fn);
}

void DiagServer::set_metrics(BodyProvider fn) { metrics_ = std::move(fn); }

void DiagServer::set_tail(TailProvider fn) { tail_ = std::move(fn); }

void DiagServer::stop() {
    const bool had_socket = listen_sock_ != ~uintptr_t{0};
    if (!had_socket && !thread_.joinable()) return;
    stop_.store(true, std::memory_order_relaxed);
    if (had_socket) {
        ::closesocket(static_cast<SOCKET>(listen_sock_));   // unblocks accept()
        listen_sock_ = ~uintptr_t{0};
    }
    if (thread_.joinable()) thread_.join();
    // Tear down live SSE streams: closing each socket unblocks a stalled
    // send/sleep so the thread sees stop_ and returns; then join. run_stream
    // never closes its own socket, so each is closed exactly once here.
    {
        std::lock_guard<std::mutex> lk(streams_mu_);
        for (auto& st : streams_) ::closesocket(static_cast<SOCKET>(st->sock));
        for (auto& st : streams_)
            if (st->th.joinable()) st->th.join();
        streams_.clear();
    }
    if (had_socket) WSACleanup();
}

void DiagServer::accept_loop() {
    running_.store(true, std::memory_order_release);
    while (!stop_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        int len = sizeof peer;
        const SOCKET c = ::accept(static_cast<SOCKET>(listen_sock_),
                                  reinterpret_cast<sockaddr*>(&peer), &len);
        if (c == INVALID_SOCKET) {
            if (stop_.load(std::memory_order_relaxed)) break;   // stop() closed the socket
            continue;                                           // transient; keep serving
        }
        serve(static_cast<uintptr_t>(c));
    }
    running_.store(false, std::memory_order_release);
}

void DiagServer::serve(uintptr_t client) {
    const SOCKET s = static_cast<SOCKET>(client);
    DWORD tmo = 3000;   // recv timeout so a stalled client can't wedge the loop
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);

    std::string req;
    if (!read_request(s, req)) {
        ::closesocket(s);
        return;
    }

    std::string method, target;
    parse_request_line(req, method, target);
    const std::string path = target.substr(0, target.find('?'));

    // Control routes (POST /control/<action>): opt-in, guarded by a SEPARATE
    // token, POST-only so no browser prefetch or GET can trip a state change.
    if (path.rfind("/control/", 0) == 0) {
        if (control_token_.empty() || !control_)
            send_all(s, http_response(404, "Not Found", "text/plain", "control disabled\n"));
        else if (method != "POST")
            send_all(s, http_response(405, "Method Not Allowed", "text/plain",
                                      "control actions are POST-only\n"));
        else if (header_bearer(req) != control_token_ &&
                 query_param(target, "token") != control_token_)
            send_all(s, http_response(401, "Unauthorized", "application/json",
                                      "{\"error\":\"unauthorized\"}"));
        else
            send_all(s, http_response(200, "OK", "application/json",
                                      control_(path.substr(9))));   // strlen("/control/")
        ::closesocket(s);
        return;
    }

    if (method != "GET") {
        send_all(s, http_response(405, "Method Not Allowed", "text/plain", "GET only\n"));
        ::closesocket(s);
        return;
    }

    // The HTML shell carries no secrets and needs to load in a browser that
    // can't set an Authorization header, so it is served without auth; its JS
    // forwards the page URL's ?token= to /diag, which is guarded below.
    if (path == "/" || path == "/index.html") {
        send_all(s, http_response(200, "OK", "text/html; charset=utf-8",
                                  root_ ? root_() : "<!doctype html><title>diag</title>"));
        ::closesocket(s);
        return;
    }
    if (path == "/favicon.ico") {
        send_all(s, http_response(204, "No Content", "text/plain", ""));
        ::closesocket(s);
        return;
    }

    const bool authed = !token_.empty() && (header_bearer(req) == token_ ||
                                             query_param(target, "token") == token_);
    if (!authed) {
        send_all(s, http_response(401, "Unauthorized", "application/json",
                                  "{\"error\":\"unauthorized\"}"));
        ::closesocket(s);
        return;
    }

    if (path == "/events") {
        if (!tail_) {
            send_all(s, http_response(404, "Not Found", "text/plain", "events disabled\n"));
            ::closesocket(s);
            return;
        }
        // Resume point: Last-Event-ID header (EventSource reconnect) or ?since=,
        // else 0 (recent tail). Hand the socket to a stream thread and return to
        // accept() immediately so control/diag stay responsive.
        uint64_t start = 0;
        std::string resume = header_value(req, "last-event-id");
        if (resume.empty()) resume = query_param(target, "since");
        if (!resume.empty()) {
            try { start = std::stoull(resume); } catch (...) { start = 0; }
        }
        std::lock_guard<std::mutex> lk(streams_mu_);
        reap_streams();
        if (streams_.size() >= kMaxStreams) {
            send_all(s, http_response(503, "Service Unavailable", "text/plain",
                                      "too many event streams\n"));
            ::closesocket(s);
            return;
        }
        auto st = std::make_unique<Stream>();
        st->sock = client;
        Stream* raw = st.get();
        st->th = std::thread([this, raw, start] {
            run_stream(raw->sock, start);
            raw->done.store(true, std::memory_order_release);
        });
        streams_.push_back(std::move(st));
        return;   // stream thread owns the socket now (do NOT close here)
    }

    if (path == "/diag") {
        send_all(s, http_response(200, "OK", "application/json", diag_ ? diag_() : "{}"));
    } else if (path == "/metrics") {
        if (metrics_)
            send_all(s, http_response(200, "OK",
                                      "text/plain; version=0.0.4; charset=utf-8",
                                      metrics_()));
        else
            send_all(s, http_response(404, "Not Found", "text/plain", "metrics disabled\n"));
    } else if (path == "/logs") {
        uint64_t since = 0;
        try {
            const std::string v = query_param(target, "since");
            if (!v.empty()) since = std::stoull(v);
        } catch (...) {
            since = 0;   // malformed cursor -> treat as first poll
        }
        send_all(s, http_response(200, "OK", "application/json",
                                  logs_ ? logs_(since) : "{}"));
    } else {
        send_all(s, http_response(404, "Not Found", "text/plain", "not found\n"));
    }
    ::closesocket(s);
}

// Stream an SSE log tail until the client leaves or the server stops. Runs on a
// dedicated per-stream thread; never touches streams_mu_ (stop()/reap own the
// socket lifecycle) so it can't deadlock with a concurrent stop().
void DiagServer::run_stream(uintptr_t client, uint64_t start_cursor) {
    const SOCKET s = static_cast<SOCKET>(client);
    const std::string head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Accel-Buffering: no\r\n\r\n"
        "retry: 3000\n\n";   // browser reconnect backoff
    if (!send_all(s, head)) return;

    uint64_t cursor = start_cursor;
    int idle = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        std::string payload = tail_ ? tail_(cursor) : std::string();
        if (!payload.empty()) {
            if (!send_all(s, payload)) return;   // client gone
            idle = 0;
        } else if (++idle >= 20) {               // ~10 s: comment keeps proxies open
            if (!send_all(s, ": keepalive\n\n")) return;
            idle = 0;
        }
        // ~500 ms cadence, but wake often so stop() is noticed promptly.
        for (int i = 0; i < 5 && !stop_.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Join and drop streams whose thread has finished. Caller holds streams_mu_.
void DiagServer::reap_streams() {
    for (auto it = streams_.begin(); it != streams_.end();) {
        if ((*it)->done.load(std::memory_order_acquire)) {
            if ((*it)->th.joinable()) (*it)->th.join();
            ::closesocket(static_cast<SOCKET>((*it)->sock));
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace tt::ui
