#include "net/ipc_client.h"

#include "nlohmann/json.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstring>

using json = nlohmann::json;
using namespace std::chrono;

namespace tt::net {

namespace {

constexpr uint16_t MSG_HELLO = 0x01, MSG_REQ_CANDLES = 0x02, MSG_RESP_CANDLES = 0x03,
                   MSG_SUB_QUOTES = 0x04, MSG_SUB_ACK = 0x05, MSG_TICKS = 0x06,
                   MSG_UNSUB = 0x07, MSG_ERROR = 0x08, MSG_PING = 0x09, MSG_PONG = 0x0A,
                   MSG_SHUTDOWN = 0x0F;
constexpr uint32_t MAX_PAYLOAD = 16u * 1024 * 1024;
constexpr int SPAWN_TIMEOUT_MS = 15000;
constexpr auto PING_EVERY = seconds(5);
constexpr auto RX_DEAD_AFTER = seconds(16);

struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};

void ensure_wsa() { static WsaInit init; }

#pragma pack(push, 1)
struct TickRecord {
    uint32_t sub_id;
    uint32_t sym_idx;
    int64_t ts_ms;
    double price;
    double day_volume;
};
#pragma pack(pop)
static_assert(sizeof(TickRecord) == 32);

} // namespace

IpcClient::IpcClient(std::string python_cmd, std::string service_dir)
    : python_cmd_(std::move(python_cmd)), service_dir_(std::move(service_dir)) {}

IpcClient::~IpcClient() { stop(); }

void IpcClient::log(std::string msg) {
    if (cbs_.on_log) cbs_.on_log(std::move(msg));
}

void IpcClient::start(Callbacks cbs) {
    cbs_ = std::move(cbs);
    running_ = true;
    worker_ = std::thread([this] { worker(); });
}

void IpcClient::stop() {
    if (!running_.exchange(false)) return;
    if (connected_) send_frame(MSG_SHUTDOWN, nullptr, 0);
    {
        // Only unblock the reader; the worker thread owns all teardown, so
        // handles are never closed from two threads at once.
        std::lock_guard lock(send_mu_);
        if (sock_ != ~uintptr_t{0}) ::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
    }
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------- lifecycle

void IpcClient::worker() {
    int backoff_ms = 500;
    while (running_) {
        int port = 0;
        if (spawn_sidecar(port) && connect_socket(port)) {
            backoff_ms = 500;
            connected_ = true;
            conn_gen_.fetch_add(1);
            log("feed: connected on port " + std::to_string(port));
            read_loop();  // returns when the connection dies or stop() is called
            connected_ = false;
        }
        teardown();
        if (!running_) break;
        log("feed: down, respawning in " + std::to_string(backoff_ms) + " ms");
        for (int waited = 0; running_ && waited < backoff_ms; waited += 100)
            Sleep(100);
        backoff_ms = std::min(backoff_ms * 2, 10000);
    }
    teardown();
}

bool IpcClient::spawn_sidecar(int& port_out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return false;
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = python_cmd_ + " \"" + service_dir_ + "\\main.py\" --port 0";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, service_dir_.c_str(), &si, &pi);
    CloseHandle(pipe_write);
    if (!ok) {
        CloseHandle(pipe_read);
        log("feed: failed to launch '" + python_cmd_ + "' (is Python on PATH?)");
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    stdout_read_ = pipe_read;

    // Kill-on-close Job Object: if the terminal dies for any reason, the OS
    // reaps the sidecar with it — no orphaned python.exe.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(job_, pi.hProcess);
    }

    // Read stdout lines until TT_PORT=<n> (or timeout).
    std::string acc;
    const auto deadline = steady_clock::now() + milliseconds(SPAWN_TIMEOUT_MS);
    port_out = 0;
    while (steady_clock::now() < deadline && running_) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_read, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            DWORD code = 0;
            if (GetExitCodeProcess(proc_, &code) && code != STILL_ACTIVE) {
                log("feed: sidecar exited during startup (code " + std::to_string(code) + ")");
                return false;
            }
            Sleep(50);
            continue;
        }
        char buf[512];
        DWORD got = 0;
        if (!ReadFile(pipe_read, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        acc.append(buf, got);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.rfind("TT_PORT=", 0) == 0) {
                port_out = std::atoi(line.c_str() + 8);
            } else if (!line.empty()) {
                log(line);
            }
        }
        if (port_out) break;
    }
    if (!port_out) {
        log("feed: no TT_PORT handshake from sidecar");
        return false;
    }

    // Keep draining sidecar stdout into the log console.
    if (stdout_thread_.joinable()) stdout_thread_.join();
    HANDLE drain = pipe_read;
    stdout_thread_ = std::thread([this, drain, acc]() mutable {
        char buf[512];
        DWORD got = 0;
        while (ReadFile(drain, buf, sizeof(buf), &got, nullptr) && got > 0) {
            acc.append(buf, got);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r')) line.pop_back();
                if (!line.empty()) log(line);
            }
        }
    });
    return true;
}

bool IpcClient::connect_socket(int port) {
    ensure_wsa();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        log("feed: connect failed");
        return false;
    }
    DWORD timeout_ms = 500;  // reader wakes regularly for heartbeat bookkeeping
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    {
        std::lock_guard lock(send_mu_);
        sock_ = static_cast<uintptr_t>(s);
    }
    return send_json(MSG_HELLO, R"({"proto":1,"impl":"tt_terminal/0.1"})");
}

void IpcClient::teardown() {
    connected_ = false;
    {
        std::lock_guard lock(send_mu_);
        if (sock_ != ~uintptr_t{0}) {
            closesocket(static_cast<SOCKET>(sock_));
            sock_ = ~uintptr_t{0};
        }
    }
    if (proc_) {
        if (WaitForSingleObject(proc_, 2000) == WAIT_TIMEOUT)
            TerminateProcess(proc_, 1);
        CloseHandle(proc_);
        proc_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);  // kill-on-close reaps any stragglers
        job_ = nullptr;
    }
    if (stdout_read_) {
        CloseHandle(stdout_read_);  // unblocks the drain thread's ReadFile
        stdout_read_ = nullptr;
    }
    if (stdout_thread_.joinable()) stdout_thread_.join();
}

// ------------------------------------------------------------------- wire

bool IpcClient::send_frame(uint16_t type, const void* payload, uint32_t len) {
    std::lock_guard lock(send_mu_);
    if (sock_ == ~uintptr_t{0}) return false;
    const SOCKET s = static_cast<SOCKET>(sock_);

    char hdr[8];
    std::memcpy(hdr, &len, 4);
    std::memcpy(hdr + 4, &type, 2);
    hdr[6] = hdr[7] = 0;

    auto send_all = [s](const char* data, int n) {
        int off = 0;
        while (off < n) {
            const int sent = ::send(s, data + off, n - off, 0);
            if (sent <= 0) return false;
            off += sent;
        }
        return true;
    };
    if (!send_all(hdr, 8)) return false;
    return len == 0 || send_all(static_cast<const char*>(payload), static_cast<int>(len));
}

bool IpcClient::send_json(uint16_t type, const std::string& text) {
    return send_frame(type, text.data(), static_cast<uint32_t>(text.size()));
}

void IpcClient::read_loop() {
    const SOCKET s = static_cast<SOCKET>(sock_);
    auto last_rx = steady_clock::now();
    auto last_ping = last_rx;

    // Reads exactly n bytes; short reads spin (frames arrive promptly).
    // Returns false on connection death; timeouts run heartbeat bookkeeping.
    auto recv_exact = [&](char* dst, int n) -> bool {
        int off = 0;
        while (off < n && running_) {
            const int got = ::recv(s, dst + off, n - off, 0);
            if (got > 0) {
                off += got;
                last_rx = steady_clock::now();
                continue;
            }
            if (got == 0) return false;  // orderly close
            if (WSAGetLastError() != WSAETIMEDOUT) return false;
            const auto now = steady_clock::now();
            if (now - last_ping > PING_EVERY) {
                last_ping = now;
                send_frame(MSG_PING, nullptr, 0);
            }
            if (now - last_rx > RX_DEAD_AFTER) {
                log("feed: heartbeat lost");
                return false;
            }
        }
        return off == n;
    };

    std::vector<char> payload;
    while (running_) {
        char hdr[8];
        if (!recv_exact(hdr, 8)) return;
        uint32_t len;
        uint16_t type;
        std::memcpy(&len, hdr, 4);
        std::memcpy(&type, hdr + 4, 2);
        if (len > MAX_PAYLOAD) {
            log("feed: oversized frame, dropping connection");
            return;
        }
        payload.resize(len);
        if (len && !recv_exact(payload.data(), static_cast<int>(len))) return;
        dispatch(type, payload);
    }
}

void IpcClient::dispatch(uint16_t type, const std::vector<char>& payload) {
    switch (type) {
    case MSG_HELLO:
    case MSG_PONG:
        break;
    case MSG_RESP_CANDLES: {
        if (payload.size() < 4) return;
        uint32_t meta_len;
        std::memcpy(&meta_len, payload.data(), 4);
        if (payload.size() < 4 + meta_len) return;
        json meta = json::parse(payload.begin() + 4, payload.begin() + 4 + meta_len,
                                nullptr, false);
        if (meta.is_discarded()) return;

        CandleBatch batch;
        batch.id = meta.value("id", 0u);
        batch.symbol = meta.value("symbol", "");
        batch.interval = meta.value("interval", "");
        batch.cached = meta.value("cached", false);
        const size_t body = payload.size() - 4 - meta_len;
        const size_t count = body / sizeof(Candle);
        batch.candles.resize(count);
        if (count)
            std::memcpy(batch.candles.data(), payload.data() + 4 + meta_len,
                        count * sizeof(Candle));
        if (cbs_.on_candles) cbs_.on_candles(std::move(batch));
        break;
    }
    case MSG_SUB_ACK: {
        json ack = json::parse(payload.begin(), payload.end(), nullptr, false);
        if (ack.is_discarded()) return;
        const uint32_t req_id = ack.value("id", 0u);
        const uint32_t sub_id = ack.value("sub", 0u);
        std::vector<std::string> by_idx;
        if (ack.contains("symbols")) {
            by_idx.resize(ack["symbols"].size());
            for (auto& [sym, idx] : ack["symbols"].items()) {
                const size_t i = idx.get<size_t>();
                if (i < by_idx.size()) by_idx[i] = sym;
            }
        }
        // Single-active-subscription semantics: the watchlist re-subscribes
        // with its full symbol list, so any older sub is now obsolete.
        std::vector<uint32_t> stale;
        {
            std::lock_guard lock(subs_mu_);
            pending_subs_.erase(req_id);
            for (const auto& [old_id, _] : subs_)
                if (old_id != sub_id) stale.push_back(old_id);
            subs_[sub_id] = std::move(by_idx);
            for (uint32_t old_id : stale) subs_.erase(old_id);
        }
        for (uint32_t old_id : stale) {
            json req = {{"sub", old_id}};
            send_json(MSG_UNSUB, req.dump());
        }
        break;
    }
    case MSG_TICKS: {
        for (size_t off = 0; off + sizeof(TickRecord) <= payload.size();
             off += sizeof(TickRecord)) {
            TickRecord rec;
            std::memcpy(&rec, payload.data() + off, sizeof(rec));
            std::string symbol;
            {
                std::lock_guard lock(subs_mu_);
                auto it = subs_.find(rec.sub_id);
                if (it != subs_.end() && rec.sym_idx < it->second.size())
                    symbol = it->second[rec.sym_idx];
            }
            if (!symbol.empty() && cbs_.on_tick)
                cbs_.on_tick(symbol, Quote{rec.price, rec.ts_ms, rec.day_volume});
        }
        break;
    }
    case MSG_ERROR: {
        json err = json::parse(payload.begin(), payload.end(), nullptr, false);
        if (err.is_discarded()) return;
        if (cbs_.on_error)
            cbs_.on_error(err.value("id", 0u), err.value("code", ""), err.value("message", ""));
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------- API

uint32_t IpcClient::request_candles(const std::string& symbol, const std::string& interval,
                                    const std::string& range) {
    if (!connected_) return 0;
    const uint32_t id = next_id_.fetch_add(1);
    json req = {{"id", id}, {"symbol", symbol}, {"interval", interval}, {"range", range}};
    return send_json(MSG_REQ_CANDLES, req.dump()) ? id : 0;
}

uint32_t IpcClient::subscribe_quotes(const std::vector<std::string>& symbols, int poll_s) {
    if (!connected_) return 0;
    const uint32_t id = next_id_.fetch_add(1);
    {
        std::lock_guard lock(subs_mu_);
        pending_subs_[id] = symbols;
    }
    json req = {{"id", id}, {"symbols", symbols}, {"poll_s", poll_s}};
    return send_json(MSG_SUB_QUOTES, req.dump()) ? id : 0;
}

void IpcClient::unsubscribe(uint32_t sub_id) {
    if (!connected_) return;
    json req = {{"sub", sub_id}};
    send_json(MSG_UNSUB, req.dump());
    std::lock_guard lock(subs_mu_);
    subs_.erase(sub_id);
}

} // namespace tt::net
