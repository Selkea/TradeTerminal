#include "net/diag_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <string_view>

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

void send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(s, data.data() + off,
                             static_cast<int>(data.size() - off), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
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

// Value of ?token=<x> (or &token=) in the request target, or "".
std::string query_token(std::string_view target) {
    const size_t q = target.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view qs = target.substr(q + 1);
    for (size_t i = 0; i < qs.size();) {
        const size_t amp = qs.find('&', i);
        std::string_view kv = qs.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == "token")
            return std::string(kv.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return {};
}

} // namespace

DiagServer::~DiagServer() { stop(); }

bool DiagServer::start(const std::string& host, uint16_t port, std::string token,
                       BodyProvider diag, BodyProvider root, LogFn on_log) {
    if (running()) return true;
    token_ = std::move(token);
    diag_ = std::move(diag);
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

void DiagServer::stop() {
    const bool had_socket = listen_sock_ != ~uintptr_t{0};
    if (!had_socket && !thread_.joinable()) return;
    stop_.store(true, std::memory_order_relaxed);
    if (had_socket) {
        ::closesocket(static_cast<SOCKET>(listen_sock_));   // unblocks accept()
        listen_sock_ = ~uintptr_t{0};
    }
    if (thread_.joinable()) thread_.join();
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

    const bool authed =
        !token_.empty() && (header_bearer(req) == token_ || query_token(target) == token_);
    if (!authed) {
        send_all(s, http_response(401, "Unauthorized", "application/json",
                                  "{\"error\":\"unauthorized\"}"));
        ::closesocket(s);
        return;
    }

    if (path == "/diag") {
        send_all(s, http_response(200, "OK", "application/json", diag_ ? diag_() : "{}"));
    } else {
        send_all(s, http_response(404, "Not Found", "text/plain", "not found\n"));
    }
    ::closesocket(s);
}

} // namespace tt::ui
