#include "net/status_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <random>

namespace tt::ui {

namespace {

std::string random_token() {
    std::random_device rd;
    static constexpr char hex[] = "0123456789abcdef";
    std::string t(16, '0');
    for (char& c : t) c = hex[rd() & 0xf];
    return t;
}

// Best-effort LAN IPv4 for the "type this on your phone" URL. Finding it via
// a connected UDP socket avoids enumerating adapters (no packet is sent).
std::string lan_ip() {
    std::string ip = "<pc-ip>";
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return ip;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == 0) {
        sockaddr_in local{};
        int len = sizeof local;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf)) ip = buf;
        }
    }
    ::closesocket(s);
#endif
    return ip;
}

// Phone-sized, dark, zero dependencies; polls /api/status every second.
constexpr const char* kPage = R"html(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TradeTerminal</title>
<style>
 body{margin:0;font:15px system-ui,sans-serif;background:#0f1115;color:#d8dbe2}
 .wrap{max-width:520px;margin:0 auto;padding:14px}
 h1{font-size:17px;margin:2px 0 10px;color:#8ab4f8}
 .cards{display:grid;grid-template-columns:1fr 1fr;gap:8px}
 .card{background:#181b22;border-radius:8px;padding:10px}
 .k{font-size:11px;color:#7a8394;text-transform:uppercase;letter-spacing:.4px}
 .v{font-size:19px;margin-top:2px;font-variant-numeric:tabular-nums}
 .pos{color:#4cc38a}.neg{color:#e5484d}.warn{color:#f0b429}
 table{width:100%;border-collapse:collapse;margin-top:12px;font-size:13px}
 th{text-align:left;color:#7a8394;font-weight:500;padding:4px 6px;
    border-bottom:1px solid #262b35}
 td{padding:4px 6px;border-bottom:1px solid #1c212b;
    font-variant-numeric:tabular-nums}
 .stale{opacity:.45}
 #state{font-size:12px;padding:2px 8px;border-radius:10px;background:#262b35;
        vertical-align:2px;margin-left:8px}
</style></head><body><div class="wrap">
<h1>TradeTerminal<span id="state">connecting…</span></h1>
<div class="cards">
 <div class="card"><div class="k">Equity</div><div class="v" id="eq">–</div></div>
 <div class="card"><div class="k">Cash</div><div class="v" id="cash">–</div></div>
 <div class="card"><div class="k">Ticks</div><div class="v" id="ticks">–</div></div>
 <div class="card"><div class="k">Tick&#8594;order p50/p99</div><div class="v" id="lat">–</div></div>
</div>
<table id="postab"><thead><tr><th>Symbol</th><th>Last</th><th>Pos</th></tr></thead>
<tbody></tbody></table>
<table id="ordtab"><thead><tr><th>#</th><th>Side</th><th>Qty</th><th>Px</th><th>Status</th></tr></thead>
<tbody></tbody></table>
</div><script>
const tok=new URLSearchParams(location.search).get('t')||'';
const fmt=n=>n.toLocaleString(undefined,{maximumFractionDigits:2});
async function tick(){
 try{
  const r=await fetch('/api/status?t='+tok);
  if(!r.ok){document.getElementById('state').textContent='HTTP '+r.status;return}
  const s=await r.json();
  const st=document.getElementById('state');
  st.textContent=s.running?(s.halted?'HALTED':'LIVE'):'idle';
  st.style.color=s.running?(s.halted?'#f0b429':'#4cc38a'):'#7a8394';
  document.getElementById('eq').textContent=fmt(s.equity);
  document.getElementById('cash').textContent=fmt(s.cash);
  document.getElementById('ticks').textContent=s.ticks.toLocaleString();
  document.getElementById('lat').textContent=s.lat_count?
    (s.lat_p50_us.toFixed(1)+' / '+s.lat_p99_us.toFixed(1)+' µs'):'–';
  document.querySelector('#postab tbody').innerHTML=s.symbols.map(x=>
   `<tr><td>${x.symbol}</td><td>${fmt(x.last)}</td><td class="${x.pos>0?'pos':x.pos<0?'neg':''}">${fmt(x.pos)}</td></tr>`).join('');
  document.querySelector('#ordtab tbody').innerHTML=s.orders.map(o=>
   `<tr class="${o.status==='working'?'':'stale'}"><td>${o.id}</td><td class="${o.side==='buy'?'pos':'neg'}">${o.side}</td><td>${fmt(o.qty)}</td><td>${o.fill_price?fmt(o.fill_price):(o.limit_price?fmt(o.limit_price):'mkt')}</td><td>${o.status}</td></tr>`).join('');
  document.body.style.opacity=1;
 }catch(e){document.getElementById('state').textContent='offline'}
}
tick();setInterval(tick,1000);
</script></body></html>)html";

const char* status_name(uint8_t s) {
    switch (static_cast<tt::OrderStatus>(s)) {
    case tt::OrderStatus::Working: return "working";
    case tt::OrderStatus::Filled: return "filled";
    case tt::OrderStatus::Cancelled: return "cancelled";
    default: return "rejected";
    }
}

} // namespace

StatusServer::StatusServer(Engine& engine, int port)
    : engine_(engine), port_(port), token_(random_token()),
      server_(std::make_unique<httplib::Server>()) {}

StatusServer::~StatusServer() { stop(); }

std::string StatusServer::url() const {
    return "http://" + lan_ip() + ":" + std::to_string(port_) + "/?t=" + token_;
}

std::string StatusServer::status_json() const {
    const LiveSnapshot s = engine_.live_snapshot();
    nlohmann::json j;
    j["running"] = s.running;
    j["halted"] = s.halted;
    j["equity"] = s.equity;
    j["cash"] = s.cash;
    j["ticks"] = s.ticks;
    j["dropped"] = s.dropped_ticks;
    j["lat_p50_us"] = s.lat_p50 / 1000.0;
    j["lat_p99_us"] = s.lat_p99 / 1000.0;
    j["lat_count"] = s.lat_count;
    auto& syms = j["symbols"] = nlohmann::json::array();
    for (const SymbolState& sym : s.symbols)
        syms.push_back({{"symbol", sym.symbol},
                        {"last", sym.last_price},
                        {"pos", sym.position.qty}});
    auto& orders = j["orders"] = nlohmann::json::array();
    const size_t first = s.orders.size() > 20 ? s.orders.size() - 20 : 0;
    for (size_t i = s.orders.size(); i-- > first;) {   // newest first
        const OrderRecord& o = s.orders[i];
        orders.push_back({{"id", o.id},
                          {"side", o.side == static_cast<uint8_t>(Side::Buy) ? "buy" : "sell"},
                          {"qty", o.qty},
                          {"limit_price", o.limit_price},
                          {"fill_price", o.fill_price},
                          {"status", status_name(static_cast<uint8_t>(o.status))}});
    }
    return j.dump();
}

bool StatusServer::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    auto authed = [this](const httplib::Request& req) {
        return req.get_param_value("t") == token_;
    };
    server_->Get("/", [this, authed](const httplib::Request& req, httplib::Response& res) {
        if (!authed(req)) {
            res.status = 403;
            res.set_content("missing or bad token", "text/plain");
            return;
        }
        res.set_content(kPage, "text/html");
    });
    server_->Get("/api/status",
                 [this, authed](const httplib::Request& req, httplib::Response& res) {
                     if (!authed(req)) {
                         res.status = 403;
                         return;
                     }
                     res.set_content(status_json(), "application/json");
                 });

    if (!server_->bind_to_port("0.0.0.0", port_)) return false;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { serve(); });
    return true;
}

void StatusServer::serve() {
    server_->listen_after_bind();   // blocks until stop()
    running_.store(false, std::memory_order_release);
}

void StatusServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

} // namespace tt::ui
