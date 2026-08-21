#include "app.h"
#include "alert_rules.h"   // classify_alert: what pages the phone
#include "block_wait.h"    // wait_off_core: sub-ms sleep_for does not sleep here
#include "build_info.h"   // TT_GIT_COMMIT / TT_GIT_DIRTY, stamped at build time
#include "dev_paths.h"
#include "market_calendar.h"   // is_us_trading_day: the pre-open check may not act on a holiday

#include "imgui_internal.h"  // DockBuilder API (default first-run layout) + private dock node flags
#include "implot.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA (gateway login page / launch)
#endif

#include "engine/ack_latency.h"
#include "engine/version.h"
#include "net/feed_order.h"   // which symbol may lose tick-by-tick

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <utility>

namespace tt::ui {

namespace {
// Destroy a broker adapter off the render thread. TWS's connect call is a
// blocking, uninterruptible network call (see tws_broker.cpp); if the old I/O
// thread is mid-(re)connect, its destructor's join can take arbitrarily long.
// The render thread must never wait on that — so hand the object to a
// detached thread and return immediately. The caller's pointer is moved-from
// (null) before this returns, so nothing on the render thread can observe or
// touch the old object again regardless of how long its real teardown takes.
//
// `outstanding` (optional) is incremented on hand-off and decremented once the
// destructor has actually returned, so a caller can WAIT for the teardown to
// finish instead of guessing. With the orders client id fixed at
// kTwsOrdersClientId, the lineup swap's restart-on-the-next-frame (~16 ms) would
// otherwise race our own socket: the old I/O thread can sit in waitForSignal for
// up to a second before it sees stop_ and calls eDisconnect, so the new session
// would draw "error 326: client id already in use" from a connection this
// process has not finished closing. That is a self-inflicted collision and the
// fix is to not inflict it, rather than to retry through it.
template <class T>
void reap_async(std::unique_ptr<T> obj,
                std::shared_ptr<std::atomic<int>> outstanding = nullptr) {
    if (!obj) return;
    if (outstanding) outstanding->fetch_add(1, std::memory_order_acq_rel);
    std::thread([o = std::move(obj), outstanding]() mutable {
        o.reset();
        if (outstanding) outstanding->fetch_sub(1, std::memory_order_acq_rel);
    }).detach();
}

// Monotonic milliseconds, for ages that must survive a clock change. The
// gateway restarts nightly and the VPS re-syncs NTP around it; a wall-clock
// jump there would show up as hours of fake bar staleness and page the phone.
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Optimizer/autopilot interval strings, indexed by SweepPanel::Settings::
// interval_idx (same table as panels/sweep.cpp's kIntervals). Named once
// because /diag, /metrics and the history watchdog must all measure the SAME
// series the autopilot re-fetches — pick a different one and the staleness
// figure is about a series nothing refreshes.
const char* sweep_interval_str(int idx) {
    static constexpr const char* kIvl[] = {"5m", "1h", "1d"};
    return kIvl[std::clamp(idx, 0, 2)];
}

// Fill-sim order latency: once the live broker has logged at least this many
// acks, backtest/optimizer/replay use the measured median + spread; until then
// they use the realistic VPS default (~75 ms), not the 250 us ExecParams toy.
constexpr uint64_t kMinLatSamples = 20;
constexpr int64_t kDefaultLatNs = 75'000'000;
constexpr int64_t kDefaultLatJitterNs = 25'000'000;

// {base_ns, jitter_ns} for ExecParams fills: measured live latency once enough
// acks are in, else the realistic VPS default.
std::pair<int64_t, int64_t> sim_exec_latency(const AppConfig& c) {
    if (static_cast<uint64_t>(c.measured_lat_count) >= kMinLatSamples &&
        c.measured_lat_ns > 0)
        return {c.measured_lat_ns, c.measured_lat_jitter_ns};
    return {kDefaultLatNs, kDefaultLatJitterNs};
}

// 128-bit random bearer token as 32 lowercase hex chars, for the diagnostics
// endpoint. std::random_device is fine here: this guards a read-only monitor on
// an encrypted tailnet, not a cryptographic key.
std::string gen_diag_token() {
    std::random_device rd;
    std::uniform_int_distribution<int> hex(0, 15);
    static const char* d = "0123456789abcdef";
    std::string t(32, '0');
    for (char& c : t) c = d[hex(rd)];
    return t;
}

// Compact, locale-independent number for Prometheus values: up to 10
// significant figures, no exponent for the ranges we emit (equity, counts,
// fractions), no trailing-zero noise.
std::string fmt_num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

// epoch seconds -> "YYYY-MM-DDTHH:MM:SSZ" (UTC), for /diag timestamps.
std::string iso_utc(std::time_t t) {
    if (t == 0) return "";
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

const char* order_type_name(uint8_t t) {
    switch (static_cast<OrdType>(t)) {
        case OrdType::Market: return "market";
        case OrdType::Limit:  return "limit";
        case OrdType::Stop:   return "stop";
    }
    return "?";
}

// Self-contained monitoring page served at GET /. Carries no data itself: its
// JS forwards the page URL's ?token= to /diag and renders the JSON, refreshing
// every 2 s. Kept inline (no external assets) so it works over a bare tailnet.
const char* diag_html() {
    return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TradeTerminal diag</title>
<style>
 body{font:13px/1.5 ui-monospace,Consolas,monospace;background:#0f1216;color:#d7dde3;margin:0;padding:16px}
 h1{font-size:15px;margin:14px 0 4px;color:#8ab4f8}
 .muted{color:#7a848f}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:12px 0}
 .card{background:#171b21;border:1px solid #232a32;border-radius:6px;padding:8px 10px}
 .k{color:#7a848f;font-size:11px;text-transform:uppercase;letter-spacing:.04em}
 .v{font-size:16px}
 .bad{color:#ff6b6b}.good{color:#57d38c}.warn{color:#e6b455}
 table{border-collapse:collapse;width:100%;margin-top:6px}
 th,td{text-align:left;padding:3px 8px;border-bottom:1px solid #232a32}
 th{color:#7a848f;font-weight:600}
 pre{white-space:pre-wrap;background:#171b21;border:1px solid #232a32;border-radius:6px;padding:8px;max-height:40vh;overflow:auto}
 #err{color:#ff6b6b}
</style></head><body>
<h1 style="margin-top:0">TradeTerminal — diagnostics</h1>
<div class="muted" id="sub">connecting…</div>
<div id="err"></div>
<div id="ctl" style="display:none;margin:10px 0">
 <button id="killbtn" style="background:#7a1f1f;color:#fff;border:1px solid #ff6b6b;border-radius:6px;padding:8px 14px;font:inherit;cursor:pointer">&#9940; KILL SWITCH — flatten &amp; halt</button>
 <span class="muted">asks for the control token</span>
</div>
<div class="grid" id="cards"></div>
<h1>Positions</h1>
<table id="pos"><thead><tr><th>Symbol</th><th>Pos</th><th>Avg</th><th>Last</th><th>uPnL</th><th>Strategy</th><th>Guard</th></tr></thead><tbody></tbody></table>
<h1>Recent rejects</h1>
<table id="rej"><thead><tr><th>Id</th><th>Symbol</th><th>Side</th><th>Type</th><th>Qty</th><th>Limit</th><th>Reason</th></tr></thead><tbody></tbody></table>
<h1>Log <span class="muted" id="logstat"></span></h1>
<pre id="log"></pre>
<h1 class="muted">Raw /diag</h1>
<pre id="raw"></pre>
<script>
const q=location.search;
function card(k,v,cls){return '<div class="card"><div class="k">'+k+'</div><div class="v '+(cls||'')+'">'+v+'</div></div>';}
function fmt(n,d){return (typeof n==='number')?n.toFixed(d===undefined?2:d):n;}
function esc(s){return String(s==null?'':s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
async function tick(){
 try{
  const r=await fetch('/diag'+q,{cache:'no-store'});
  if(!r.ok){document.getElementById('err').textContent='HTTP '+r.status+(r.status==401?' — add ?token=... to the URL':'');return;}
  document.getElementById('err').textContent='';
  const d=await r.json();
  document.getElementById('sub').textContent=d.git_commit+(d.git_dirty?'*':'')+' · '+d.route+' · up '+d.uptime_sec+'s · '+d.now;
  document.getElementById('ctl').style.display=(d.control==='enabled')?'block':'none';
  let c='';
  c+=card('Live',d.live_running?'RUNNING':'idle',d.live_running?'good':'muted');
  c+=card('Halted',d.halted?'HALTED':'no',d.halted?'bad':'good');
  c+=card('Equity','$'+fmt(d.equity));
  c+=card('Cash','$'+fmt(d.cash));
  c+=card('Unprotected',d.unprotected_positions,d.unprotected_positions>0?'bad':'good');
  if(d.live_running&&d.risk&&d.risk.nearest_halt!=='none'){const f=d.risk.nearest_halt_frac;c+=card('Halt room',Math.max(0,Math.round((1-f)*100))+'% ('+d.risk.nearest_halt+')',f>0.8?'bad':f>0.5?'warn':'good');}
  c+=card('Rejects',d.reject_count,d.reject_count>0?'warn':'good');
  c+=card('Feed stale',(d.feed_stale_ms<0?'—':d.feed_stale_ms+' ms'),d.feed_stale_ms>10000?'warn':'');
  if(d.stuck_orders>0)c+=card('Half-open',d.stuck_orders,'warn');
  c+=card('Dropped ticks',d.dropped_ticks,d.dropped_ticks>0?'warn':'');
  c+=card('Ack p50',fmt(d.latency.ack_p50_ms)+' ms');
  c+=card('Ack p90',fmt(d.latency.ack_p90_ms)+' ms');
  document.getElementById('cards').innerHTML=c;
  let pb='';
  for(const s of d.symbols){
   const guard=s.position==0?'flat':(s.unprotected?'NAKED':'ok');
   pb+='<tr><td>'+s.symbol+'</td><td>'+fmt(s.position,0)+'</td><td>'+fmt(s.avg_price)+'</td><td>'+fmt(s.last_price)+'</td><td class="'+(s.unrealized_pnl<0?'bad':'good')+'">'+fmt(s.unrealized_pnl)+'</td><td>'+(s.strategy||'')+'</td><td class="'+(s.unprotected?'bad':'good')+'">'+guard+'</td></tr>';
  }
  document.querySelector('#pos tbody').innerHTML=pb||'<tr><td colspan=7 class=muted>none</td></tr>';
  let rb='';
  for(const x of d.rejects_recent){const reason=((x.reject_code?x.reject_code+' ':'')+(x.reject_msg||'')).trim()||'—';rb+='<tr><td>'+x.id+'</td><td>'+esc(x.symbol)+'</td><td>'+x.side+'</td><td>'+x.type+'</td><td>'+fmt(x.qty,0)+'</td><td>'+fmt(x.limit_price)+'</td><td class=bad>'+esc(reason)+'</td></tr>';}
  document.querySelector('#rej tbody').innerHTML=rb||'<tr><td colspan=7 class=muted>none</td></tr>';
  document.getElementById('raw').textContent=JSON.stringify(d,null,2);
 }catch(e){document.getElementById('err').textContent=String(e);}
}
tick();setInterval(tick,2000);
// Kill switch: confirm, then prompt for the control token (kept out of the URL
// and history), then POST /control/kill with it.
document.getElementById('killbtn').onclick=async()=>{
 if(!confirm('Flatten ALL positions, cancel open orders, and HALT trading now?'))return;
 const tok=prompt('Control token (from config.json diag_control_token):');
 if(!tok)return;
 try{
  const r=await fetch('/control/kill?token='+encodeURIComponent(tok),{method:'POST'});
  const t=await r.text();
  alert(r.ok?('Kill sent — '+t):('FAILED: HTTP '+r.status+' '+t));
 }catch(e){alert('Error: '+e);}
};
// Live log tail: prefer the SSE stream (/events, sub-second) and fall back to
// polling /logs if EventSource is unavailable or never connects. Both share
// logCursor, and the stream stamps events with id:=cursor, so a fallback or an
// EventSource reconnect resumes with no gap or duplicate dump.
let logCursor=0;const logEl=document.getElementById('log');
function stat(t){const st=document.getElementById('logstat');if(st)st.textContent=t;}
function appendLog(text,cls){
 const atBottom=logEl.scrollHeight-logEl.scrollTop-logEl.clientHeight<40;
 const div=document.createElement('div');if(cls)div.className=cls;div.textContent=text;logEl.appendChild(div);
 while(logEl.childNodes.length>1500)logEl.removeChild(logEl.firstChild);
 if(atBottom)logEl.scrollTop=logEl.scrollHeight;
}
let pollTimer=null;
async function logTick(){
 try{
  const sep=q?'&':'?';
  const r=await fetch('/logs'+q+sep+'since='+logCursor,{cache:'no-store'});
  if(!r.ok)return;
  const d=await r.json();
  if(d.dropped)appendLog('… older lines dropped','muted');
  for(const ln of d.lines)appendLog(ln);
  logCursor=d.next;stat('#'+logCursor+' (poll)');
 }catch(e){}
}
function startPoll(){if(pollTimer)return;logTick();pollTimer=setInterval(logTick,1500);}
function stopPoll(){if(pollTimer){clearInterval(pollTimer);pollTimer=null;}}
if(window.EventSource){
 let opened=false;
 const es=new EventSource('/events'+q);
 es.onopen=()=>{opened=true;stopPoll();stat('live');};
 es.onmessage=(e)=>{if(e.lastEventId)logCursor=+e.lastEventId;for(const ln of e.data.split('\n'))appendLog(ln);};
 es.addEventListener('dropped',()=>appendLog('… older lines dropped','muted'));
 es.onerror=()=>{if(!opened)startPoll();};        // never connected -> poll; else EventSource auto-retries
 setTimeout(()=>{if(!opened)startPoll();},4000);   // backstop if onerror never fires
}else{startPoll();}
</script>
</body></html>
)HTML";
}

std::filesystem::path data_dir() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(base ? base : ".") / "TradeTerminal";
}
std::string strategies_out_dir() { return (data_dir() / "strategies").string(); }
std::string sessions_dir() { return (data_dir() / "sessions").string(); }
std::string gxx_path() {
    const char* env = std::getenv("TT_GXX");
    return env ? env : TT_GXX_DEFAULT;
}

// PowerShell arguments to run the IBeam auto-login helper, or "" if the script
// is missing. Launches a headless browser login against the running gateway
// using credentials saved (DPAPI-encrypted) by scripts\Save-IbkrCred.ps1.
std::string ps_script_path(const char* name) {
    std::error_code ec;
    auto script = std::filesystem::path(TT_REPO_ROOT) / "scripts" / name;
    if (std::filesystem::exists(script, ec)) return script.make_preferred().string();
    return {};
}
// PowerShell argument string for one of the repo scripts, or "" if missing.
std::string ps_args(const char* name, bool hidden = true, const std::string& extra = {}) {
    const std::string p = ps_script_path(name);
    if (p.empty()) return {};
    std::string a = "-NoProfile ";
    if (hidden) a += "-WindowStyle Hidden ";
    a += "-ExecutionPolicy Bypass -File \"" + p + "\"";
    if (!extra.empty()) a += " " + extra;
    return a;
}
std::string signout_args() { return ps_args("Stop-IbkrLogin.ps1"); }
std::string switch_args(const std::string& acct) {
    return ps_args("Switch-IbkrAccount.ps1", true, "-Account \"" + acct + "\"");
}
std::string addnew_args() { return ps_args("Save-IbkrCred.ps1", /*hidden=*/false); }

// Labels + paper flag + active label from the multi-account IBKR credential
// store. Labels/paper are plaintext; usernames/passwords are DPAPI-encrypted.
struct IbkrAccount {
    std::string name;        // unique key: switch/remove act on this
    std::string label;       // display name (defaults to name)
    bool paper = true;
    bool readonly = false;   // live login for viewing/testing; block all orders
};
struct IbkrAccountList {
    std::string active;
    std::vector<IbkrAccount> accounts;

    // Is the active account flagged read-only?
    bool active_readonly() const {
        for (const auto& a : accounts)
            if (a.name == active) return a.readonly;
        return false;
    }
};
IbkrAccountList read_ibkr_accounts() {
    IbkrAccountList r;
    std::ifstream f(data_dir() / "ibkr-accounts.json");
    if (!f) return r;
    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return r;
    r.active = j.value("active", std::string());
    for (const auto& a : j.value("accounts", nlohmann::json::array())) {
        auto nm = a.value("name", std::string());
        if (nm.empty()) continue;
        IbkrAccount acc;
        acc.label = a.value("label", nm);   // display name; defaults to the key
        acc.name = std::move(nm);
        acc.paper = a.value("paper", true);
        acc.readonly = a.value("readonly", false);
        r.accounts.push_back(std::move(acc));
    }
    return r;
}

// Is the ACTIVE IBKR account a paper one? UNKNOWN READS AS LIVE — no store, an
// unparseable store, or an `active` name matching no entry all answer false.
// The only caller uses this to decide whether it may COLD-restart IB Gateway,
// and a cold re-login on a live account can stop dead on a 2FA prompt IBC
// cannot type (which is why Start-IbGateway.ps1 keeps live accounts on the soft
// AutoRestartTime). "I don't know" must not authorize that.
//
// Deliberately not folded into tws_api_port() below, which defaults the other
// way: picking port 4002 for an unknown account is a harmless wrong guess,
// killing a live gateway is not.
bool active_account_is_paper() {
    const auto ib = read_ibkr_accounts();
    for (const auto& a : ib.accounts)
        if (a.name == ib.active) return a.paper;
    return false;
}

// IB Gateway socket port follows the active account's mode.
int tws_api_port() {
    const auto ib = read_ibkr_accounts();
    bool paper = true;
    for (const auto& a : ib.accounts)
        if (a.name == ib.active) paper = a.paper;
    int port = paper ? 4002 : 4001;
    if (const char* p = std::getenv("TT_TWS_PORT")) port = std::atoi(p);
    return port;
}

#ifdef _WIN32
// Launch "powershell.exe <args>" with no console window at all. ShellExecute's
// SW_HIDE can still flash a console for console-subsystem apps; CREATE_NO_WINDOW
// does not, and child processes inherit the no-window state. wait_ms > 0 blocks
// up to that many ms for it to finish (0 = fire-and-forget). Kept short on the
// shutdown path so a slow cleanup script can't stall the exit and hold the
// single-instance mutex; the child keeps running independently either way.
void run_hidden(const std::string& args, unsigned wait_ms = 0) {
    std::string cmd = "powershell.exe " + args;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (wait_ms) WaitForSingleObject(pi.hProcess, wait_ms);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif

// Colored "PAPER" (green) / "LIVE" (red) badge on the current line.
void badge_paper_live(bool paper) {
    ImGui::SameLine();
    if (paper)
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "PAPER");
    else
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1), "LIVE");
}
void badge_kind(net::AccountKind k) {
    using K = net::AccountKind;
    if (k == K::Paper) badge_paper_live(true);
    else if (k == K::Live) badge_paper_live(false);
    // Unknown: no badge
}
} // namespace

std::string App::polygon_key() const {
    if (auto a = accounts_.active("polygon")) return a->key_id;
    const char* k = std::getenv("POLYGON_API_KEY");
    return k && *k ? k : "";
}

std::string App::finnhub_key() const {
    if (auto a = accounts_.active("finnhub")) return a->key_id;
    const char* k = std::getenv("FINNHUB_API_KEY");
    return k && *k ? k : "";
}

App::App(std::string gateway_url)
    : config_path_((data_dir() / "config.json").string()),
      cfg_(AppConfig::load(config_path_)),
      gw_(std::move(gateway_url)),
      use_tws_data_(cfg_.trade_route == 1),
      data_(use_tws_data_ ? static_cast<net::IMarketData&>(tws_data_)
                          : static_cast<net::IMarketData&>(gw_)),
      host_(gxx_path(), std::string(TT_REPO_ROOT) + "/sdk/include", strategies_out_dir()),
      chart_(data_, series_),
      watchlist_(data_, quotes_),
      backtest_(engine_),
      replay_(engine_, sessions_dir()),
      strat_mgr_(host_, engine_, std::string(TT_REPO_ROOT) + "/strategies"),
      trade_(engine_, quotes_),
      blotter_(engine_),
      positions_(engine_, quotes_),
      sweep_panel_(engine_),
      accounts_((data_dir() / "accounts.json").string()) {
    net::IMarketData::Callbacks cbs;
    // alert_scan FIRST, exactly as every other adapter's pop_log pump does. The
    // market-data source was the ONE adapter whose log lines bypassed the alert
    // classifier, which is why the 2026-08-11 client-id collision — seven
    // repetitions of a fatal, permanent error — reached the log file and
    // nothing else. There is no webhook, no /events entry and no beep for a
    // failure that leaves the app with no candles at all.
    cbs.on_log = [this](std::string line) {
        alert_scan(line);
        route(std::move(line));
    };
    cbs.on_tick = [this](const std::string& sym, const Quote& q) {
        quotes_.set(sym, q);
        // When the real-time feed owns the session, delayed sidecar quotes
        // still update the watchlist but must not reach the engine.
        if (engine_.live_running() && !rt_feed_active_.load(std::memory_order_relaxed))
            engine_.push_live_tick(sym, q.ts_ms, q.price, q.day_volume);
    };
    cbs.on_error = [this](uint32_t id, std::string code, std::string msg) {
        route("feed error (req " + std::to_string(id) + ") " + code + ": " + msg);
        // Only the request that actually failed. This used to clear the pending
        // backtest on ANY feed error, which was both too broad (an unrelated
        // symbol's error cancelled the panel's run) and too narrow: it never
        // touched the pending SWEEP, so a tournament candidate whose fetch was
        // errored out by drop_connection went on waiting for bars that could
        // never arrive until its whole 60s budget expired. Six of those in a row
        // is five wasted minutes per symbol, and the 2026-08-10 build spent most
        // of its 25 minutes exactly that way.
        {
            std::lock_guard lock(pending_bt_mu_);
            if (pending_bt_.active && pending_bt_.req_id == id)
                pending_bt_.active = false;
            if (sweep_setup_.waiting && sweep_setup_.req_id == id) {
                sweep_setup_.waiting = false;
                sweep_setup_.ready = false;
                sweep_setup_.req_id = 0;
            }
        }
        // Every OTHER waiter on a request id, for the same reason. Each of these
        // used to be released only by a timer, so a request the feed had already
        // given up on still cost its waiter the full wait. Separate scopes, never
        // nested: these locks have no ordering relationship with each other.
        {
            std::lock_guard<std::mutex> g(lineup_mu_);
            const auto it = lineup_want_bars_.find(id);
            if (it != lineup_want_bars_.end()) {
                lineup_dropped_.push_back(it->second);
                lineup_want_bars_.erase(it);
            }
        }
        {
            std::lock_guard lock(warmup_mu_);
            // A warmup that will never arrive. Dropping the ticket is the whole
            // fix: left in place it is claimed by the NEXT delivery of some other
            // request, which reseeds from whatever that one happened to fetch.
            const auto it = warmup_want_.find(id);
            if (it != warmup_want_.end()) {
                route("live: WARNING " + it->second.symbol +
                      " warmup history failed - it trades this session on "
                      "tick-aggregated bars only");
                warmup_want_.erase(it);
            }
        }
    };
    cbs.on_candles = [this](net::CandleBatch&& b) {
        route("candles: " + b.symbol + " " + b.interval + " x" +
                 std::to_string(b.candles.size()) + (b.cached ? " (cache)" : ""));
        // The only place a history fetch is known to have SUCCEEDED. A failing
        // request is cancelled at the 20s mark and leaves the pending set, so
        // this is the sole signal that can see the 2026-08-07 stall. No source
        // sets `cached` on delivery today, but if one ever replays a cache
        // instead of asking IB it must not count as proof the session works.
        // Nor does an EMPTY delivery: on the ibkr_web route a gateway that has
        // lost the conid's market-data entitlement answers 200 with
        // {"data":[]}, ibkr_parse_history_bars accepts it, and the batch that
        // reaches here carries no candles at all. Counting that as a refresh
        // would reset the metric on the very failure it exists to catch, while
        // the tournament scores nothing and seed_bars re-seeds nothing.
        // ...and the NEWEST BAR's own timestamp alongside the delivery time. A
        // delivery age answers "is the request path alive"; only the bar's stamp
        // answers "is the data advancing", and on 2026-08-14 STKH's 5m series
        // sat frozen at exactly x10010 bars for five hours while its deliveries
        // arrived punctually — the watchdog reported it healthy the whole time
        // and /diag published the delivery age under the name last_bar_age_ms.
        // Candle::ts is epoch SECONDS; the bar map is wall-clock ms.
        if (!b.cached && !b.candles.empty())
            hist_fresh_.record(b.symbol, b.interval, steady_ms(),
                               b.candles.back().ts * 1000);
        start_pending_backtest(b);
        stash_pending_sweep(b);
        collect_lineup_bars(b);   // before the move: needs b.candles
        const std::string sym = b.symbol;
        const std::string ivl = b.interval;
        const uint32_t bid = b.id;
        series_.put(b.symbol, b.interval, std::move(b.candles), b.cached);
        // A live symbol that started cold can now be warmed (after the put, so
        // seed_bars sees this batch). Matched on the REQUEST ID this warmup was
        // asked under — see App::warmup_want_ for the ticket a same-symbol chart
        // fetch used to steal.
        uint32_t want_sid = 0;
        {
            std::lock_guard lock(warmup_mu_);
            const auto it = warmup_want_.find(bid);
            if (it != warmup_want_.end()) {
                // The symbol check is belt and braces: an id that is ours cannot
                // carry another symbol unless the id space is corrupt, and
                // reseeding the wrong symbol's engine slot would be far worse
                // than not reseeding at all.
                if (it->second.symbol == sym) want_sid = it->second.sid;
                else
                    route("live: DISCARDING warmup req " + std::to_string(bid) +
                          " - asked for " + it->second.symbol + ", got " + sym);
                warmup_want_.erase(it);
            }
        }
        if (want_sid && engine_.live_running()) {
            std::vector<tt::Bar> bars = seed_bars(sym, cfg_.trade_bar_sec);
            if (!bars.empty()) {
                route("live: " + sym + ": warmup history arrived (" +
                         std::to_string(bars.size()) + " bars, " + ivl + ")");
                engine_.reseed_symbol(want_sid, std::move(bars));
            }
        }
    };

    // Session persistence (cfg_ is loaded in the init list) + file logging.
    log_.set_log_file((data_dir() / "logs" / "terminal.log").string());
    opt_log_.set_log_file((data_dir() / "logs" / "optimizer.log").string());
    watchlist_.set_symbols(cfg_.watchlist);
    chart_.restore(cfg_.chart_symbol, cfg_.chart_interval_idx, cfg_.chart_range_idx);
    backtest_.set_cash(cfg_.backtest_cash);
    trade_.restore(cfg_.trade_cash, cfg_.trade_bar_sec, cfg_.trade_data_idx,
                   cfg_.trade_record, cfg_.trade_route);
    trade_.restore_schedule(cfg_.trade_sched_on, cfg_.trade_sched_start,
                            cfg_.trade_sched_stop, cfg_.trade_sched_blocked_day);
    if (!cfg_.lineup_build_time.empty() &&
        cfg_.lineup_build_time.size() < sizeof lineup_build_buf_)
        std::snprintf(lineup_build_buf_, sizeof lineup_build_buf_, "%s",
                      cfg_.lineup_build_time.c_str());
    // TT_AUTORUN_LINEUP=1: this process is a propose-only daily-lineup DRY RUN.
    // Latched once, here, so every guard downstream tests one flag rather than
    // re-reading the environment (and so the mode cannot change mid-run).
    dry_.active = std::getenv("TT_AUTORUN_LINEUP") != nullptr;
    {
        RiskLimits r;
        r.max_order_qty = cfg_.risk_max_order_qty;
        r.max_position_qty = cfg_.risk_max_position_qty;
        r.daily_max_loss = cfg_.risk_daily_max_loss;
        r.stale_feed_sec = cfg_.risk_stale_feed_sec;
        r.disable_auto_halt = cfg_.risk_disable_halt;
        trade_.restore_risk(r, cfg_.risk_max_drawdown_pct);
    }
    trade_.restore_symbols(cfg_.trade_symbols);
    backtest_.set_strategy(cfg_.backtest_strategy);
    replay_.restore(cfg_.replay_strategy, cfg_.replay_cash, cfg_.replay_bar_sec);
    sweep_panel_.restore({cfg_.sweep_strategy, cfg_.sweep_symbol,
                          cfg_.sweep_interval_idx, cfg_.sweep_range_idx, cfg_.sweep_cash,
                          cfg_.sweep_metric, cfg_.sweep_holdout, cfg_.sweep_holdout_pct});
    // Panel visibility from last session (missing entry = the panel's default).
    {
        auto vis = [&](const char* k, bool& flag) {
            const auto it = cfg_.panels.find(k);
            if (it != cfg_.panels.end()) flag = it->second;
        };
        vis("chart", show_chart_);
        vis("watchlist", show_watchlist_);
        vis("backtest", show_backtest_);
        vis("replay", show_replay_);
        vis("optimizer", show_sweep_);
        vis("strategy", show_strategy_);
        vis("build_output", show_build_output_);
        vis("trade", show_trade_);
        vis("blotter", show_blotter_);
        vis("positions", show_positions_);
        vis("journal", show_journal_);
        vis("log", show_log_);
        vis("optlog", show_opt_log_);
    }
    // Rebuild last session's strategies and their params.
    //
    // ONE-TIME 0.16.1 UPGRADE: drop the saved per-strategy parameter map instead
    // of restoring it. Every build up to 0.16.0 wrote each tournament CANDIDATE
    // sweep's in-sample winner into that map — before holdout scoring, with no
    // rollback on rejection — so what a fresh install would treat as "the
    // strategy's defaults" is, on any existing install, a pile of unadjudicated
    // fits belonging to whichever symbol was optimized last. A tab with no set
    // of its own inherits exactly this (2026-08-10: six symbols on one SSPC
    // fit), and no rule downstream can tell a deliberate default from the
    // residue. Handing restore_state an empty map makes adopt_params fall back
    // to each strategy's own declared default for every name, which is what the
    // fallback is documented to be. Deliberate values are cheap to set again and
    // are now the only thing that ever lands there.
    const bool purge_params = !cfg_.strategy_params_purged;
    if (purge_params) {
        int n_keys = 0, n_vals = 0;
        for (const auto& [k, m] : cfg_.strategy_params) {
            if (m.empty()) continue;
            ++n_keys;
            n_vals += static_cast<int>(m.size());
        }
        route("strategies: 0.16.1 upgrade — discarded " + std::to_string(n_vals) +
                 " saved parameter value(s) across " + std::to_string(n_keys) +
                 " strategies. Pre-0.16.1 builds let every tournament candidate "
                 "write that shared map, so its contents were fits on whatever "
                 "symbol was optimized last, not defaults. Reset to each "
                 "strategy's declared defaults; re-run the Optimizer to set any "
                 "you meant to keep.");
        cfg_.strategy_params.clear();
        cfg_.strategy_params_purged = true;
    }
    strat_mgr_.restore_state(cfg_.strategy_loaded, cfg_.strategy_params,
                             cfg_.strategy_tourn_excluded);
    const char* wh = std::getenv("TT_ALERT_WEBHOOK");
    alerts_.set_webhook(wh && *wh ? wh : cfg_.alert_webhook);
    // The gate lives in AlertNotifier (one place, every caller), but the POLICY
    // deciding which category a line belongs to stays here in the UI layer —
    // alerts.cpp must not pull in the engine's tag headers just to mute a fill.
    alerts_.set_categorizer([](const std::string& l) {
        return static_cast<int>(classify_alert_category(l));
    });
    apply_notification_settings();
    if (alerts_.has_webhook()) route("alerts: webhook configured");

    if (!journal_.open((data_dir() / "journal.db").string()))
        route("journal: could not open journal.db — history disabled");

#ifdef TT_DEBUG
    sim_ticks_ = std::getenv("TT_SIM_TICKS") != nullptr;
#endif

    if (use_tws_data_) {
        tws_data_.set_endpoint("127.0.0.1", tws_api_port());
        tws_data_.start(std::move(cbs));
        route("data: TWS route - charts/watchlist/backtests ride IB Gateway; "
                 "CP web gateway stays down (one brokerage session per username)");
    } else {
        gw_.start(std::move(cbs));
    }

#ifdef _WIN32
    // Tie the broker gateway to this app's lifetime. Web route: CP gateway +
    // IBeam auto-login, stopped again in the destructor. TWS route: IB Gateway
    // via IBC, driven by a keepalive daemon (Watch-IbGateway.ps1) that starts it
    // if needed, re-logs-in if a login fails at a bad moment (e.g. IBKR's
    // overnight maintenance window - the one-shot launch had no retry), and
    // exits when THIS app closes (it watches -AppPid). The gateway itself is
    // left running on exit so the login survives an app restart.
    {
        const auto ib = read_ibkr_accounts();
        if (!ib.accounts.empty() && !ib.active.empty()) {
            const std::string args =
                use_tws_data_
                    ? ps_args("Watch-IbGateway.ps1", true,
                              "-AppPid " + std::to_string(GetCurrentProcessId()))
                    : ps_args("Start-IbkrLogin.ps1", true, "-Daemon");
            if (!args.empty()) {
                run_hidden(args);
                gateway_starting_until_ = 90.0;   // show INITIALIZING until it connects
                route(use_tws_data_
                             ? "account: starting IB Gateway (TWS route)"
                             : "account: starting IBKR gateway + auto-login (tied to app)");
            }
        }
    }
#endif

    session_start_ = std::time(nullptr);
    start_diag_server();
    // Poll GitHub in the background for a newer main than this build; the update
    // panel only appears once available() flips true. Falls back to VERSION-file
    // comparison when the build has no git commit, so it still works then.
    update_.start(TT_REPO_SLUG, TT_GIT_COMMIT, TT_VERSION_BASE);
}

// IPC thread: if this candle batch is the one a queued backtest is waiting
// for, convert it and launch the engine. The lock is held through the engine
// start so the UI thread's lease GC can never observe "pending consumed +
// engine idle" while the leased instance is being handed to the engine.
void App::start_pending_backtest(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    // By request id, not by symbol + interval: see SweepSetup::req_id.
    if (!pending_bt_.active || pending_bt_.req_id == 0 ||
        pending_bt_.req_id != batch.id)
        return;
    if (pending_bt_.symbol != batch.symbol || pending_bt_.interval != batch.interval) {
        // Cannot happen: the source stamps the batch with the pub_id of the
        // request that asked for it. If it ever does, the id space has been
        // corrupted and running a backtest on these bars would be the 2026-08-10
        // failure again, so refuse loudly rather than quietly.
        route("backtest: DISCARDING req " + std::to_string(batch.id) + " - asked for " +
              pending_bt_.symbol + " " + pending_bt_.interval + ", got " + batch.symbol +
              " " + batch.interval);
        pending_bt_.active = false;
        return;
    }
    pending_bt_.active = false;   // GC-safe: engine start happens under the lock
    if (batch.candles.size() < 3) {
        route("backtest: not enough data for " + batch.symbol);
        return;
    }
    BacktestConfig cfg;
    cfg.symbol = batch.symbol;
    cfg.bars.reserve(batch.candles.size());
    for (const Candle& c : batch.candles)
        cfg.bars.push_back(Bar{c.ts * 1'000'000'000, c.open, c.high, c.low,
                               c.close, c.volume});
    cfg.initial_cash = pending_bt_.cash;
    cfg.params = std::move(pending_bt_.params);
    // Fills use the live-measured order latency when we have it, else the VPS
    // default — never the 250 us ExecParams toy (unrealistic for scalping).
    const auto [bt_lat, bt_jit] = sim_exec_latency(cfg_);
    cfg.exec.latency_ns = bt_lat;
    cfg.exec.latency_jitter_ns = bt_jit;
    route(static_cast<uint64_t>(cfg_.measured_lat_count) >= kMinLatSamples
                 ? "backtest fill latency: measured " +
                       std::to_string(bt_lat / 1'000'000) + "+/-" +
                       std::to_string(bt_jit / 1'000'000) + " ms (" +
                       std::to_string(cfg_.measured_lat_count) + " live acks)"
                 : "backtest fill latency: default 75 ms (no live acks yet)");
    if (!engine_.start_backtest(std::move(cfg), pending_bt_.strategy))
        route("backtest: engine busy, try again");
}

// UI thread: lease a fresh instance of `key`, capture params, fetch data.
void App::queue_backtest(const std::string& key, const std::string& sym,
                         const std::string& ivl, const std::string& rng,
                         double cash) {
    if (!data_.connected()) {
        route("backtest: feed is down, cannot fetch data");
        return;
    }
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        route("backtest: strategy '" + key + "' is not loaded");
        return;
    }
    leases_.push_back({inst, key, StrategyLease::Backtest});
    // The lock is held ACROSS request_candles so the id is in place before any
    // delivery can be matched against it. Lock order is pending_bt_mu_ ->
    // TwsData::mu_ and never the reverse: the data thread takes pending_bt_mu_
    // from on_candles with no source lock held (pump_requests releases mu_ before
    // it delivers, cache hit or not), so the two can never close a cycle.
    std::lock_guard lock(pending_bt_mu_);
    pending_bt_ = {true, 0, sym, ivl, strat_mgr_.param_values(key), cash, inst};
    pending_bt_.req_id = data_.request_candles(sym, ivl, rng);
    if (pending_bt_.req_id == 0) {
        pending_bt_.active = false;
        route("backtest: feed would not accept the request");
    }
}

// "" is an alias for the promoted sma_crossover.cpp -- one implementation,
// not a separately hand-maintained "built-in" class that could drift from it
// (see terminal/CMakeLists.txt's TT_PROMOTED_STRATEGIES). Any other promoted
// key resolves the same way; otherwise an instance from a hot-loaded DLL module.
IStrategy* App::acquire_strategy(const std::string& key) {
    const std::string& k = key.empty() ? kBuiltinStrategyKey : key;
    if (const tt::StaticStrategyEntry* e = tt::find_static_strategy(k)) return e->create();
    return host_.create_instance(k);
}

void App::release_strategy(const StrategyLease& lease) {
    if (lease.key.empty() || tt::find_static_strategy(lease.key))
        lease.inst->destroy();
    else
        host_.destroy_instance(lease.inst);
}

// UI thread, per frame: destroy leased instances whose run can no longer
// touch them. Conditions are conservative — a lease outliving its run by a
// few frames is fine; destroying early is the segfault class this exists to
// prevent.
void App::pump_leases() {
    for (size_t i = 0; i < leases_.size();) {
        const StrategyLease& l = leases_[i];
        bool done = false;
        switch (l.kind) {
        case StrategyLease::Live:
            done = !engine_.live_running();
            break;
        case StrategyLease::Backtest: {
            bool referenced;
            {
                std::lock_guard lock(pending_bt_mu_);
                referenced = pending_bt_.active && pending_bt_.strategy == l.inst;
            }
            done = !referenced && !engine_.running();
            break;
        }
        case StrategyLease::Sweep: {
            bool referenced;
            {
                std::lock_guard lock(pending_bt_mu_);
                referenced = (sweep_setup_.waiting || sweep_setup_.ready) &&
                             sweep_setup_.strategy == l.inst;
            }
            const bool sweep_active =
                sweep_strategy_ == l.inst && (sweep_.running || sweep_holdout_phase_);
            done = !referenced && !sweep_active && !engine_.running();
            break;
        }
        }
        if (!done) {
            ++i;
            continue;
        }
        if (sweep_strategy_ == l.inst) sweep_strategy_ = nullptr;
        release_strategy(l);
        leases_[i] = leases_.back();
        leases_.pop_back();
    }
}

// UI thread: run with a specific strategy source, building + loading its
// module first when it's absent or stale ("" = built-in, never builds).
void App::queue_backtest_as(const std::string& src, const std::string& sym,
                            const std::string& ivl, const std::string& rng,
                            double cash) {
    if (strat_mgr_.loaded_fresh(src)) {
        queue_backtest(src, sym, ivl, rng, cash);
        return;
    }
    pending_run_ = {true, src, sym, ivl, rng, cash};
    strat_mgr_.request_load(src);
    route("backtest: building " + src + " first");
}

// UI thread, per frame: fire the deferred backtest once the module is
// loaded, or drop it when the build/load failed.
void App::pump_pending_run() {
    if (!pending_run_.active) return;
    if (strat_mgr_.loaded_fresh(pending_run_.src)) {
        pending_run_.active = false;
        queue_backtest(pending_run_.src, pending_run_.symbol,
                       pending_run_.interval, pending_run_.range,
                       pending_run_.cash);
    } else if (!strat_mgr_.load_pending()) {
        pending_run_.active = false;
        route("backtest: strategy build failed — see the Strategy panel");
    }
}

// ------------------------------------------------------------------ sweep

// UI thread: capture the request + strategy and fetch the data.
//
// Returns TRUE only when a fetch is genuinely outstanding — i.e. sweep_setup_ is
// armed and waiting. Every `return false` below leaves sweep_setup_ exactly as it
// was, which pump_tournament has to be able to tell apart from "the fetch came
// back with nothing": its Queued phase reads !sweep_fetch_pending() as "settled",
// and the previous candidate left waiting==false, so a candidate that was never
// requested at all was written off on the very next frame.
bool App::queue_sweep(const SweepPanel::Request& rq, bool for_tournament) {
    if (!data_.connected()) {
        route("sweep: feed is down, cannot fetch data");
        return false;
    }
    if (sweep_.running || engine_.running()) {
        route("sweep: engine busy, try again");
        return false;
    }
    const std::string key = rq.strat_key;
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        route("sweep: strategy '" + strat_mgr_.display_name(key) + "' is not loaded");
        return false;
    }
    leases_.push_back({inst, key, StrategyLease::Sweep});
    // Held ACROSS request_candles: the id has to be recorded before the data
    // thread can deliver against it, and with net/bar_cache.h a hit is delivered
    // on the very next io_loop pass — the window is real, not theoretical. See
    // queue_backtest for the lock ordering that makes holding it here safe.
    std::lock_guard lock(pending_bt_mu_);
    sweep_setup_ = SweepSetup{};
    sweep_setup_.waiting = true;
    sweep_setup_.req = rq;
    sweep_setup_.strategy = inst;
    sweep_setup_.key = key;
    sweep_setup_.params = strat_mgr_.param_values(key);
    sweep_setup_.for_tournament = for_tournament;
    sweep_setup_.req_id = data_.request_candles(rq.symbol, rq.interval, rq.range);
    if (sweep_setup_.req_id == 0) {
        sweep_setup_.waiting = false;
        route("sweep: feed would not accept the request for " + rq.symbol);
    }
    return sweep_setup_.waiting;
}

// The caller gave up on the candles it asked for. Clear the wait so a late
// batch is ignored: stash_pending_sweep matches on symbol + interval only, so
// without this the abandoned request still starts a sweep — one that finishes
// with nobody left to own its result. Cheap and idempotent; the outstanding
// request just goes unclaimed.
void App::cancel_pending_sweep() {
    std::lock_guard lock(pending_bt_mu_);
    if (!sweep_setup_.waiting) return;
    sweep_setup_.waiting = false;
    sweep_setup_.ready = false;
    sweep_setup_.req_id = 0;
}

// UI thread. `ready` counts as pending: the bars are in but pump_sweep has not
// turned them into a running sweep yet, and reporting "settled" in that window
// would let pump_tournament write the candidate off one frame before it starts.
bool App::sweep_fetch_pending() const {
    std::lock_guard lock(pending_bt_mu_);
    return sweep_setup_.waiting || sweep_setup_.ready;
}

// IPC thread: if this batch is what the sweep is waiting for, stash the
// bars; the UI thread picks them up in pump_sweep().
void App::stash_pending_sweep(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    // The delivery is this sweep's only if it carries the id this sweep asked
    // under. See SweepSetup::req_id: the previous symbol+interval match handed a
    // 6-month optimization 1567 warmup bars and nothing anywhere said so.
    if (!sweep_setup_.waiting || sweep_setup_.req_id == 0 ||
        sweep_setup_.req_id != batch.id)
        return;
    if (sweep_setup_.req.symbol != batch.symbol ||
        sweep_setup_.req.interval != batch.interval) {
        // Unreachable unless the source mislabels its own deliveries. Refuse the
        // bars rather than optimize on them, and say so — the whole point of the
        // 2026-08-10 defect is that it was silent.
        route("sweep: DISCARDING req " + std::to_string(batch.id) + " - asked for " +
              sweep_setup_.req.symbol + " " + sweep_setup_.req.interval + " " +
              sweep_setup_.req.range + ", got " + batch.symbol + " " + batch.interval);
        sweep_setup_.waiting = false;
        sweep_setup_.req_id = 0;
        return;
    }
    sweep_setup_.bars.clear();
    sweep_setup_.bars.reserve(batch.candles.size());
    for (const Candle& c : batch.candles)
        sweep_setup_.bars.push_back(
            Bar{c.ts * 1'000'000'000, c.open, c.high, c.low, c.close, c.volume});
    sweep_setup_.waiting = false;
    sweep_setup_.ready = true;
}

// One backtest: the best-so-far values with the current param overridden by
// the current 1-D grid point.
void App::start_sweep_cell() {
    BacktestConfig cfg = sweep_base_;   // copies bars (a few MB at worst)
    cfg.params = opt_.best;
    cfg.params[opt_.params[static_cast<size_t>(opt_.pi)].name] =
        sweep_.xs[static_cast<size_t>(opt_.step)];
    if (!engine_.start_backtest(std::move(cfg), sweep_strategy_)) {
        route("optimizer: engine busy, aborted");
        sweep_.running = false;
    }
}

// Set up the 1-D grid for the current (pass, param) and run its first cell.
// Pass 0 spans the param's full declared range; later passes refine in a
// narrower window centered on the best value so far.
void App::start_opt_param() {
    const AutoOpt::Param& p = opt_.params[static_cast<size_t>(opt_.pi)];
    double lo = p.min, hi = p.max;
    if (opt_.pass > 0) {
        const double w = (p.max - p.min) * kSweepRefineWindow;
        const double c = opt_.best[p.name];
        lo = std::max(p.min, c - w / 2);
        hi = std::min(p.max, lo + w);
        lo = std::max(p.min, hi - w);
    }
    sweep_.cur_param = p.name;
    sweep_.pass = opt_.pass;
    sweep_.xs.clear();
    for (int i = 0; i < kSweepSteps; ++i)
        sweep_.xs.push_back(lo + (hi - lo) * i / (kSweepSteps - 1));
    sweep_.vals.assign(kSweepSteps, std::numeric_limits<double>::quiet_NaN());
    opt_.step = 0;
    start_sweep_cell();
}

// UI thread, before the panels draw (so a finished cell's result is consumed
// here and never stolen by the Backtest panel).
void App::pump_sweep() {
    // Fetched candles arrived: set up the grid and start cell 0.
    {
        std::lock_guard lock(pending_bt_mu_);
        if (sweep_setup_.ready) {
            sweep_setup_.ready = false;
            // Latch the provenance decided at queue time onto the sweep that is
            // about to run; sweep_setup_ is free to be reused from here on.
            sweep_for_tournament_ = sweep_setup_.for_tournament;
            const SweepPanel::Request& rq = sweep_setup_.req;
            if (sweep_setup_.bars.size() < 3) {
                route("sweep: not enough data for " + rq.symbol);
            } else {
                sweep_base_ = BacktestConfig{};
                sweep_base_.symbol = rq.symbol;
                sweep_base_.bars = std::move(sweep_setup_.bars);
                sweep_base_.initial_cash = rq.cash;
                sweep_base_.params = std::move(sweep_setup_.params);
                // Every sweep cell copies sweep_base_, so set the fill latency
                // once here (measured live latency, else the VPS default).
                {
                    const auto [sw_lat, sw_jit] = sim_exec_latency(cfg_);
                    sweep_base_.exec.latency_ns = sw_lat;
                    sweep_base_.exec.latency_jitter_ns = sw_jit;
                    route(static_cast<uint64_t>(cfg_.measured_lat_count) >= kMinLatSamples
                                 ? "optimizer fill latency: measured " +
                                       std::to_string(sw_lat / 1'000'000) + "+/-" +
                                       std::to_string(sw_jit / 1'000'000) + " ms (" +
                                       std::to_string(cfg_.measured_lat_count) + " live acks)"
                                 : "optimizer fill latency: default 75 ms (no live acks yet)");
                }
                sweep_strategy_ = sweep_setup_.strategy;

                // Walk-forward split: the newest slice is held out of the
                // optimization entirely and scores the winner afterwards.
                sweep_test_bars_.clear();
                sweep_holdout_phase_ = false;
                double holdout = rq.holdout_pct;
                if (holdout > 0) {
                    const size_t n = sweep_base_.bars.size();
                    const size_t n_train =
                        static_cast<size_t>(n * (1.0 - holdout / 100.0));
                    if (n_train >= 3 && n - n_train >= 3) {
                        sweep_test_bars_.assign(sweep_base_.bars.begin() + n_train,
                                                sweep_base_.bars.end());
                        sweep_base_.bars.resize(n_train);
                    } else {
                        holdout = 0;
                        route("sweep: too little data for a holdout, skipped");
                    }
                }

                // Coordinate descent over the strategy's declared parameters,
                // starting from its current values.
                opt_ = AutoOpt{};
                opt_.key = sweep_setup_.key;
                // Sizing and session-shape knobs keep their manual values; only
                // signal params are swept (see sweep_param_is_fixed).
                //
                // ...and time_stop is additionally CAPPED to what this bar size
                // can reach inside a trading day (sweep_param_max). A ParamDesc
                // is bar-size agnostic — bollinger_reversion declares max 500,
                // which is a legitimate swing hold on daily bars — so the bound
                // can only be applied here, where the interval is known. On
                // 2026-08-14 the uncapped range let the tournament crown a
                // 233-bar time stop on a 300 s series and score it +8.60%, on a
                // hold production flattens out of existence every afternoon.
                const int sweep_bar_sec = bar_seconds_of_interval(rq.interval);
                for (const auto& s : strat_mgr_.param_specs(opt_.key))
                    if (s.max > s.min && !sweep_param_is_fixed(s.name)) {
                        const double hi =
                            sweep_param_max(s.name, s.max, sweep_bar_sec);
                        if (hi <= s.min) continue;   // nothing left to sweep
                        opt_.params.push_back({s.name, s.min, hi});
                    }
                opt_.best = sweep_base_.params;
                // THE SOURCE FIX behind that cap: give the replay the day
                // boundary the live engine has. Engine::run otherwise walks six
                // months of bars with no session concept at all, so it scores
                // holds no live position can ever achieve — and it will keep
                // doing that for every time-based parameter of every strategy,
                // not just time_stop. Optimizer path only: a manual backtest and
                // a replay must keep judging the data's own clock.
                // See BacktestConfig::eod_flatten_h. The predicate lives in
                // sweep_eod_flatten_h so it is testable — nothing constructs an
                // App, so an expression here could be deleted with a green suite
                // and take the whole fix with it.
                sweep_base_.eod_flatten_h = sweep_eod_flatten_h(sweep_bar_sec);
                // The SAME hour gates entries, because live it is the same hour:
                // entry_cutoff_h is defined as the last minute a position may be
                // opened because it is the last minute something will still close
                // it. Scoring an entry the live gate refuses is the same class of
                // lie as scoring a hold the live backstop flattens.
                sweep_base_.entry_cutoff_h = sweep_base_.eod_flatten_h;
                // ...AND THE SIZE. The two lines above stopped the replay
                // scoring trades live refuses on the CLOCK; this one stops it
                // scoring them at a size live refuses. Engine::run built its
                // EngineCtx with no risk pointer, so ctx.budget() returned the
                // whole account: measured at 989 shares / $94,509 of $100,000
                // against a $5,000 live per-symbol budget, ~19x, on every score
                // this optimizer has ever produced. Same limits the live session
                // will use, from the same function that derives them there.
                sweep_base_.risk = sweep_risk_limits(trade_.risk(), rq.cash);

                sweep_ = SweepPanel::State{};
                sweep_.holdout_pct = holdout;
                sweep_.metric = rq.metric;
                sweep_.label = rq.symbol + " " + rq.interval + " " + rq.range + " — " +
                               strat_mgr_.display_name(sweep_setup_.key);
                if (opt_.params.empty()) {
                    route("optimizer: no tunable parameters");
                } else {
                    sweep_.running = true;
                    sweep_.n_passes = kSweepPasses;
                    sweep_.total = kSweepPasses *
                                   static_cast<int>(opt_.params.size()) * kSweepSteps;
                    start_opt_param();
                }
            }
        }
    }

    // Drain as many finished cells as the frame budget allows, instead of
    // exactly one.
    //
    // Every cell here is a ~5.84 ms backtest (measured: 582 of them in the
    // 2026-08-10 build for 3398 ms of engine CPU in total), and they already run
    // off the UI thread. But this function took ONE result per frame and
    // main.cpp asks for glfwSwapInterval(1), so each cell cost a whole vsync
    // period — observed ~30 ms wall against 5.84 ms of work. The optimizer ran at
    // the refresh rate of the monitor, on a headless VPS.
    //
    // consume_sweep_result() starts the next cell, so the poll below is waiting
    // on a backtest that is already in flight; kSweepDrainBudgetMs bounds how
    // much of the tick that may take, and is deliberately LONGER than one
    // backtest (see the constant) — a budget shorter than that would just move
    // the one-per-tick ceiling from 30 ms to 16.7 ms.
    //
    // The wait must BLOCK, not spin — the engine is on another thread and this
    // box has few cores to spare. That was written of a
    // std::this_thread::sleep_for(250us) which does not sleep at all here; see
    // block_wait.h. Measured on a replica of this loop (5.84 ms cells, the
    // 10 ms tick period, 8 ms budget): spinning cost 73.8% of a core for
    // 6.18 ms/result, blocking costs 0.0% for 6.78 ms/result. Nine percent of
    // drain throughput is the right trade for a whole core handed back to the
    // engine thread running those backtests and to the live session beside it.
    const auto drain_start = std::chrono::steady_clock::now();
    auto drain_spent_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - drain_start)
            .count();
    };
    while (sweep_.running) {
        // A mid-sweep rebuild is harmless now: the sweep's leased instance pins
        // its module until the last cell finishes.
        BacktestResult r;
        if (engine_.take_result(r)) {
            consume_sweep_result(r);
            if (drain_spent_ms() >= kSweepDrainBudgetMs) break;
            continue;
        }
        // Nothing ready. If nothing is running either, there is nothing to wait
        // for — the sweep is between states and the next tick will move it on.
        if (!engine_.running() || drain_spent_ms() >= kSweepDrainBudgetMs) break;
        wait_off_core();
    }
}

// One finished backtest, in the sweep's own state machine: score the cell, adopt
// the pass winner, and start whatever comes next (the next cell, the next param,
// or the holdout run). Split out of pump_sweep so the drain loop above can call
// it more than once per frame.
void App::consume_sweep_result(BacktestResult& r) {
    if (sweep_holdout_phase_) {   // the winner's run on unseen data
        sweep_.has_holdout = true;
        sweep_.holdout_trades = r.trades;
        sweep_.holdout_val = sweep_metric_of(r, sweep_.metric, kSweepMinHoldoutTrades);
        sweep_holdout_phase_ = false;
        sweep_.running = false;
        char buf[128];
        std::snprintf(buf, sizeof buf, "optimizer: holdout %s %.4g (last %.0f%%, unseen)",
                      kSweepMetrics[sweep_.metric], sweep_.holdout_val,
                      sweep_.holdout_pct);
        route(buf);
        // Now — and only now — does the swept symbol get this set as its OWN.
        // Same bar finish_tournament applies to a champion: a score on unseen
        // data, positive unless the metric is one we minimise. Thin holdouts are
        // noise, and on a minimised metric a zero-trade run wins outright.
        if (!sweep_fit_symbol_.empty()) {
            const bool minimize = sweep_metric_minimize(sweep_.metric);
            const bool ok = std::isfinite(sweep_.holdout_val) &&
                            sweep_.holdout_trades >= kSweepMinHoldoutTrades &&
                            (minimize || sweep_.holdout_val > 0.0);
            if (ok && trade_.merge_symbol_params(sweep_fit_symbol_, opt_.key,
                                                 sweep_fit_params_)) {
                std::string names;
                for (const auto& [k, v] : sweep_fit_params_) {
                    char kv[64];
                    std::snprintf(kv, sizeof kv, "%s%s=%.4g",
                                  names.empty() ? "" : " ", k.c_str(), v);
                    names += kv;
                }
                route("optimizer: " + sweep_fit_symbol_ +
                         " now has its OWN fit for these names - " + names);
            } else if (!ok) {
                route("optimizer: " + sweep_fit_symbol_ +
                         " keeps its previous parameters - the winner did not clear "
                         "its holdout, and an unvalidated set is what the lineup "
                         "would trade unattended tomorrow");
            }
            sweep_fit_symbol_.clear();
            sweep_fit_params_.clear();
        }
        return;
    }

    sweep_.vals[static_cast<size_t>(opt_.step)] =
        sweep_metric_of(r, sweep_.metric, kSweepMinTrades);
    ++sweep_.done;
    ++opt_.step;
    if (opt_.step < kSweepSteps) {
        start_sweep_cell();
        return;
    }

    // This param's 1-D sweep is done: adopt its best point if it doesn't
    // worsen the best metric seen so far (the current value may sit between
    // grid points, so a blind adopt could regress).
    const bool minimize = sweep_metric_minimize(sweep_.metric);
    int bi = 0;
    for (int i = 1; i < kSweepSteps; ++i) {
        const double a = sweep_.vals[static_cast<size_t>(i)];
        const double b = sweep_.vals[static_cast<size_t>(bi)];
        if (minimize ? a < b : a > b) bi = i;
    }
    const double bv = sweep_.vals[static_cast<size_t>(bi)];
    if (!opt_.metric_valid || (minimize ? bv <= opt_.best_metric
                                        : bv >= opt_.best_metric)) {
        opt_.best[opt_.params[static_cast<size_t>(opt_.pi)].name] =
            sweep_.xs[static_cast<size_t>(bi)];
        opt_.best_metric = bv;
        opt_.metric_valid = true;
    }
    sweep_.best = opt_.best;
    sweep_.best_metric = opt_.best_metric;
    sweep_.has_best = true;

    // Next param / pass.
    if (++opt_.pi >= static_cast<int>(opt_.params.size())) {
        opt_.pi = 0;
        ++opt_.pass;
    }
    if (opt_.pass < kSweepPasses) {
        start_opt_param();
        return;
    }

    // All passes done. Where the winner goes depends on WHO asked for the sweep.
    //
    // Until 0.16.0 this unconditionally stamped opt_.best into the shared
    // per-strategy map. A tournament runs one sweep per candidate through this
    // exact path, so on 2026-08-10 each of the five candidates per symbol wrote
    // its IN-SAMPLE winner there — before holdout scoring, before the champion
    // decision, and with no rollback when the champion was rejected. The last
    // sweep to finish anywhere (an SSPC run at 09:40:40) became the parameter
    // set that all six live symbols traded. A candidate sweep is not a verdict:
    // only finish_tournament decides, and it writes the SYMBOL, not the map.
    //
    // WHICH sweep this is was decided when it was QUEUED, not now: see
    // SweepSetup::for_tournament. Asking tourn_.active here misfiled any sweep
    // whose tournament had already timed out from under it.
    const bool candidate_of_tournament = sweep_for_tournament_;
    sweep_fit_symbol_.clear();
    sweep_fit_params_.clear();
    if (!candidate_of_tournament) {
        // A manual Optimizer run is the user setting this strategy's defaults —
        // keep that; it is the one deliberate writer of the shared map.
        strat_mgr_.set_param_values(opt_.key, opt_.best);
        sweep_.applied = true;
        // The swept SYMBOL gets its own copy too, so it stops depending on a map
        // every other symbol also reads — but ONLY the names this sweep actually
        // tuned, and only once the holdout has scored them (see
        // sweep_fit_params_). opt_.best also carries the strategy-wide starting
        // value for every name sweep_param_is_fixed excludes (qty, alloc_pct,
        // enter_from_h, ...); writing those onto the tab would overwrite
        // per-symbol settings this sweep was never allowed to touch.
        if (!sweep_base_.symbol.empty()) {
            for (const AutoOpt::Param& p : opt_.params) {
                const auto it = opt_.best.find(p.name);
                if (it != opt_.best.end()) sweep_fit_params_[p.name] = it->second;
            }
            if (!sweep_fit_params_.empty()) sweep_fit_symbol_ = sweep_base_.symbol;
        }
    }
    std::string bests;
    for (const auto& [k, v] : opt_.best) {
        char kv[64];
        std::snprintf(kv, sizeof kv, "%s%s=%.4g", bests.empty() ? "" : " ", k.c_str(), v);
        bests += kv;
    }
    route("optimizer: finished " + std::to_string(sweep_.done) + " backtests (" +
             sweep_.label + ") — " +
             (candidate_of_tournament
                  ? "candidate result (not applied; the tournament decides) "
                  : "applied to the " + strat_mgr_.display_name(opt_.key) +
                        " defaults ") +
             bests);

    if (sweep_.holdout_pct <= 0 || sweep_test_bars_.empty()) {
        // No holdout ran, so there is nothing to validate the per-symbol write
        // against. The strategy default is the user's own explicit action and
        // stands; the symbol's "own fit" — which the lineup will trade
        // unattended tomorrow — is not created on an in-sample score alone.
        if (!sweep_fit_symbol_.empty())
            route("optimizer: " + sweep_fit_symbol_ +
                     " keeps its previous parameters - no holdout ran, and an "
                     "in-sample winner is not a validated fit");
        sweep_fit_symbol_.clear();
        sweep_.running = false;
        return;
    }
    // Score the winner on the held-out tail it never saw.
    BacktestConfig cfg = sweep_base_;
    cfg.bars = sweep_test_bars_;
    cfg.params = opt_.best;
    if (engine_.start_backtest(std::move(cfg), sweep_strategy_)) {
        sweep_holdout_phase_ = true;
    } else {
        route("optimizer: holdout run could not start (engine busy)");
        sweep_fit_symbol_.clear();   // never validated, so never the symbol's own
        sweep_fit_params_.clear();
        sweep_.running = false;
    }
}

// Optimize every loaded strategy (plus the built-in) on the same data; the
// champion (best HOLDOUT score — never in-sample) is applied to the target
// symbol's Trade tab. One candidate optimizes at a time through the normal
// sweep pipeline.
void App::start_tournament(SweepPanel::Request rq, const std::string& target_symbol,
                           std::vector<std::string> candidates, bool for_lineup) {
    if (tourn_.active || sweep_.running || engine_.running()) {
        route("tournament: optimizer busy, try again");
        return;
    }
    // Champion selection needs unseen-data scoring; force a holdout.
    if (rq.holdout_pct <= 0) rq.holdout_pct = 25.0;
    tourn_ = Tournament{};
    tourn_.active = true;
    tourn_.base = std::move(rq);
    tourn_.target_symbol = target_symbol;
    tourn_.for_lineup = for_lineup;
    if (candidates.empty()) {
        // Default field: every loaded strategy ("" built-in included) EXCEPT
        // ones the user flagged out in the Strategies panel.
        for (const auto& k : strat_mgr_.loaded_keys())
            if (!strat_mgr_.tournament_excluded(k)) tourn_.candidates.push_back(k);
        if (tourn_.candidates.empty()) {
            route("tournament: every loaded strategy is excluded — enable at "
                     "least one in the Strategies panel");
            tourn_ = Tournament{};   // unwind: clears active
            return;
        }
    } else {
        tourn_.candidates = std::move(candidates);
    }
    tourn_.stamp_s = tourn_.start_s = mono_s();

    sweep_.tourney = {};
    sweep_.tourney.active = true;
    sweep_.tourney.total = static_cast<int>(tourn_.candidates.size());
    sweep_.tourney.symbol = tourn_.base.symbol;
    route("tournament: " + std::to_string(tourn_.candidates.size()) +
             " candidates on " + tourn_.base.symbol + " " + tourn_.base.interval + " " +
             tourn_.base.range);
}

void App::pump_tournament() {
    if (!tourn_.active) return;
    const double now = mono_s();
    sweep_.tourney.idx = static_cast<int>(tourn_.idx);
    sweep_.tourney.current = strat_mgr_.display_name(tourn_.candidates[tourn_.idx]);

    auto advance = [&](Tournament::Entry e) {
        tourn_.results.push_back(std::move(e));
        if (++tourn_.idx >= tourn_.candidates.size()) {
            finish_tournament();
        } else {
            tourn_.phase = Tournament::Phase::Launch;
            tourn_.stamp_s = now;
        }
    };

    switch (tourn_.phase) {
    case Tournament::Phase::Launch:
        // The hard wall (kTournDeadlineS). Checked HERE, between candidates,
        // because that is the only point where nothing is in flight: cutting a
        // tournament off while a sweep is running would leave exactly the unowned
        // sweep the 0.16.0 defect was about. Judge what has already scored — a
        // field of three real results is a verdict; refusing to look at them
        // because two candidates never got bars is not.
        if (!sweep_.running && now - tourn_.start_s > kTournDeadlineS) {
            route("tournament: " + tourn_.base.symbol + " hit its " +
                     std::to_string(static_cast<int>(kTournDeadlineS)) +
                     "s deadline after " + std::to_string(tourn_.idx) + " of " +
                     std::to_string(tourn_.candidates.size()) +
                     " candidates - judging the field it has");
            cancel_pending_sweep();
            finish_tournament();
            break;
        }
        if (!sweep_.running && !engine_.running()) {
            // Do not even ask while the feed is down. queue_sweep would refuse
            // without arming anything, and Phase::Queued reads an unarmed
            // sweep_setup_ as "the fetch settled with nothing usable" — so a
            // routine data-session reconnect (drop_connection then a >=3 s
            // backoff before connect_gateway, which the died-twice escalation
            // performs by design) burned every candidate of every symbol in about
            // two seconds, and the whole lineup aborted for the day. WAITING here
            // is bounded by kTournDeadlineS above, which is what that wall is for.
            if (!data_.connected()) {
                if (!tourn_.feed_wait_logged) {
                    tourn_.feed_wait_logged = true;
                    route("tournament: " + tourn_.base.symbol +
                             " waiting for the data session before launching " +
                             strat_mgr_.display_name(tourn_.candidates[tourn_.idx]));
                }
                break;
            }
            tourn_.feed_wait_logged = false;
            SweepPanel::Request rq = tourn_.base;
            rq.strat_key = tourn_.candidates[tourn_.idx];
            if (!queue_sweep(rq, /*for_tournament=*/true)) {
                // Refused for a reason waiting cannot fix (the strategy is not
                // loaded, or the source is shutting down). Score it as a
                // non-starter and move on rather than spin on it every frame.
                Tournament::Entry e;
                e.key = tourn_.candidates[tourn_.idx];
                advance(std::move(e));
                break;
            }
            // Keep the tournament banner alive (queue_sweep reset sweep_ state
            // when its candles arrive — re-assert below in Queued/Running).
            tourn_.phase = Tournament::Phase::Queued;
            tourn_.stamp_s = now;
        } else if (now - tourn_.stamp_s > 120.0) {
            route("tournament: engine stayed busy, aborting" +
                  (tourn_.target_symbol.empty() ? std::string()
                                                : " - " + tourn_.target_symbol +
                                                      " has no fit from this build"));
            cancel_pending_sweep();
            tourn_.active = false;
            tourn_.for_lineup = false;
            sweep_.tourney.active = false;
        }
        break;
    case Tournament::Phase::Queued: {
        sweep_.tourney.active = true;   // survive pump_sweep's sweep_ reset
        sweep_.tourney.total = static_cast<int>(tourn_.candidates.size());
        sweep_.tourney.idx = static_cast<int>(tourn_.idx);
        sweep_.tourney.symbol = tourn_.base.symbol;
        if (sweep_.running) {
            tourn_.phase = Tournament::Phase::Running;
            break;
        }
        // The fetch is SETTLED but no sweep came of it: the feed errored the
        // request out (on_error now clears the exact pending sweep), or the bars
        // were too thin to optimize on. Waiting out the full fetch budget for an
        // answer that has already arrived is pure dead time — 60s of it per
        // candidate, and the 2026-08-10 build did that thirty times.
        const bool settled = !sweep_fetch_pending();
        const bool starved = now - tourn_.stamp_s > kTournFetchTimeoutS;
        if (!settled && !starved) break;
        const std::string key = tourn_.candidates[tourn_.idx];
        route("tournament: " + tourn_.base.symbol + " has no bars for " +
                 strat_mgr_.display_name(key) + " (" +
                 (settled ? "the fetch came back with nothing usable"
                          : data_.connected()
                                ? "data session connected but no bars in " +
                                      std::to_string(static_cast<int>(kTournFetchTimeoutS)) + "s"
                                : "data session disconnected") +
                 ")");
        // Stop waiting for those candles. Left armed, a batch arriving after
        // the tournament ended still started this sweep — which then
        // finished with no owner and wrote its unadjudicated in-sample
        // winner into the shared per-strategy map (the 2026-08-10 defect,
        // reintroduced by this very timeout).
        cancel_pending_sweep();
        // ONE more turn, at the back of the field, before the candidate is
        // written off. On 2026-08-10 SOXL SMA Crossover was discarded here and
        // then produced holdout Sharpe +0.1213 — the only positive score in the
        // entire build — three seconds after the tournament had already declared
        // "no candidate produced a result". Accepting that result late is the
        // wrong repair: an unowned sweep completing with nobody to own it is
        // itself the 0.16.0 defect, and its bars had in any case come from a
        // different request (see SweepSetup::req_id). Giving the candidate
        // another turn is the safe shape of the same idea, and it composes with
        // net/bar_cache.h: whatever the first attempt was waiting for is CACHED
        // by the time the field comes round again, so the retry is served without
        // touching IB at all.
        //
        // Bounded twice over — once per candidate (`requeued`) and by the
        // tournament's own deadline — so a symbol whose data is simply gone
        // cannot hold the lineup open.
        const bool have_time = now - tourn_.start_s < kTournDeadlineS;
        if (have_time && tourn_.requeued.insert(key).second) {
            tourn_.candidates.push_back(key);
            sweep_.tourney.total = static_cast<int>(tourn_.candidates.size());
            route("tournament: " + strat_mgr_.display_name(key) +
                     " goes to the back of the field for one more attempt");
            ++tourn_.idx;   // cannot pass the end: we just appended
            tourn_.phase = Tournament::Phase::Launch;
            tourn_.stamp_s = now;
            break;
        }
        Tournament::Entry e;
        e.key = key;
        advance(std::move(e));
        break;
    }
    case Tournament::Phase::Running:
        if (sweep_.running) break;
        {
            Tournament::Entry e;
            e.key = tourn_.candidates[tourn_.idx];
            if (sweep_.has_best) {
                e.params = sweep_.best;
                e.holdout = sweep_.has_holdout;
                e.score = sweep_.has_holdout ? sweep_.holdout_val : sweep_.best_metric;
                // The holdout score is what crowns a champion, so a candidate
                // that barely traded on unseen data is not a candidate at all -
                // its score is noise, and on the minimised metric a zero-trade
                // run would win outright.
                const bool thin =
                    sweep_.has_holdout && sweep_.holdout_trades < kSweepMinHoldoutTrades;
                e.valid = !thin && std::isfinite(e.score);
                if (!e.valid)
                    route("tournament: " + strat_mgr_.display_name(e.key) +
                             " rejected (" + std::to_string(sweep_.holdout_trades) +
                             " holdout trades, need " +
                             std::to_string(kSweepMinHoldoutTrades) + ")");
            }
            advance(std::move(e));
        }
        break;
    }
}

void App::finish_tournament() {
    tourn_.active = false;
    const bool minimize = sweep_metric_minimize(tourn_.base.metric);
    int champ = -1;
    for (int i = 0; i < static_cast<int>(tourn_.results.size()); ++i) {
        const auto& e = tourn_.results[static_cast<size_t>(i)];
        if (!e.valid) continue;
        if (champ < 0 ||
            (minimize ? e.score < tourn_.results[static_cast<size_t>(champ)].score
                      : e.score > tourn_.results[static_cast<size_t>(champ)].score))
            champ = i;
    }

    // Publish the ranking to the Optimizer panel.
    sweep_.tourney.active = false;
    sweep_.tourney.done = true;
    sweep_.tourney.symbol = tourn_.base.symbol;
    sweep_.tourney.rows.clear();
    for (int i = 0; i < static_cast<int>(tourn_.results.size()); ++i) {
        const auto& e = tourn_.results[static_cast<size_t>(i)];
        sweep_.tourney.rows.push_back({strat_mgr_.display_name(e.key), e.score,
                                       e.holdout, e.valid, i == champ});
    }

    if (champ < 0) {
        // 2026-08-10: SOXS, MUU, KORU and SOXL each lost all five candidates to
        // candle-fetch timeouts and landed here — then traded anyway, on a set
        // fitted to SSPC. Nothing is recorded HERE: the lineup's admission runs
        // off lineup_.fitted, which only the successful install below writes, so
        // simply not reaching that line is already the failure. An autopilot
        // cycle failing is a different question anyway, handled in
        // autopilot_evaluate (it keeps the incumbent).
        route("tournament: no candidate produced a result" +
              (tourn_.target_symbol.empty() ? std::string()
                                            : " for " + tourn_.target_symbol));
        tourn_.for_lineup = false;
        return;
    }
    const auto& c = tourn_.results[static_cast<size_t>(champ)];
    // Winning the field is not the same as being worth trading. On every
    // maximised metric a non-positive champion is one that lost money (or never
    // won) on unseen data - crowning it hands the live engine a set we already
    // know does not work. Better to keep whatever is running.
    if (!minimize && !(c.score > 0.0)) {
        char rej[256];
        std::snprintf(rej, sizeof rej,
                      "tournament: best candidate %s scored %s %.4g on %s — not "
                      "positive, keeping the incumbent (%s keeps whatever set it "
                      "already had)",
                      strat_mgr_.display_name(c.key).c_str(),
                      kSweepMetrics[tourn_.base.metric], c.score,
                      tourn_.base.symbol.c_str(),
                      tourn_.target_symbol.empty() ? "the symbol"
                                                   : tourn_.target_symbol.c_str());
        route(rej);
        // Deliberately NOT recorded as fitted: a rejected champion leaves the
        // symbol with nothing from this build, which is precisely the state
        // admission has to see. The 0.16.0 draft's "did any candidate score?"
        // test called this a success and admitted the symbol on a set that was
        // never installed.
        tourn_.for_lineup = false;
        return;
    }
    // The champion was fitted to ONE instrument, so it belongs to that symbol.
    // Writing it to the shared per-strategy map as well is how a symbol with no
    // set of its own ended up trading another symbol's fit (2026-08-10), so a
    // targeted tournament writes the tab and nothing else.
    //
    // An UNTARGETED tournament is the Optimizer panel's own button: there is no
    // tab to write to, and the user pressed it, so the strategy default is both
    // the only home available and the thing they asked to set. Say plainly which
    // instrument that value was fitted to — inheriting it later is still never a
    // fit on the inheriting symbol.
    if (tourn_.target_symbol.empty()) {
        strat_mgr_.set_param_values(c.key, c.params);
        route("tournament: champion applied to the " +
                 strat_mgr_.display_name(c.key) + " shared defaults (fitted to " +
                 tourn_.base.symbol +
                 "; a symbol inheriting it has NOT been optimized for)");
    } else {
        trade_.set_symbol_strategy(tourn_.target_symbol, c.key, c.params);
        // The one place a lineup pick earns its admission. Recorded at the
        // install, so no failure path can be mistaken for this one.
        if (tourn_.for_lineup) lineup_.fitted.insert(tourn_.target_symbol);
    }
    tourn_.for_lineup = false;
    char buf[192];
    std::snprintf(buf, sizeof buf, "tournament: champion %s (%s %.4g%s) -> %s",
                  strat_mgr_.display_name(c.key).c_str(),
                  kSweepMetrics[tourn_.base.metric], c.score,
                  c.holdout ? " on holdout" : "", tourn_.target_symbol.c_str());
    route(buf);
}

// ---- daily auto-lineup ----------------------------------------------------

TradePanel::AccountInfo App::trade_account_info() {
    TradePanel::AccountInfo a;
    const auto ib = read_ibkr_accounts();
    for (const auto& x : ib.accounts)
        if (x.name == ib.active) { a.label = x.label; break; }
    a.kind = static_cast<int>(data_.account_kind());
    a.readonly = ib.active_readonly();
    a.subaccounts = data_.accounts();
    return a;
}

// Fire the daily lineup build on the configured pre-market clock: weekday,
// once per day, level-triggered inside a short window after the time so a late
// launch still catches it. On a non-propose-only build the lineup arms its own
// auto-start when it reaches Done (see pump_daily_lineup / start_live_session).
void App::pump_lineup_schedule() {
    // A dry run drives its OWN build (pump_lineup_dryrun) and must never trade.
    // The clock-driven build is the same start_daily_lineup call, but it passes
    // autostart_when_done = !cfg_.lineup_propose_only — so on a box configured
    // to auto-start, letting this fire would hand a dry run a live session. Off
    // for the whole run, regardless of what config.json says.
    if (dry_.active) return;
    if (!cfg_.lineup_enabled) return;
    // Never interrupt a running session or an in-flight build/optimize.
    if (lineup_active() || engine_.running() || tourn_.active || sweep_.running) return;
    int bh = -1, bm = -1;
    if (std::sscanf(cfg_.lineup_build_time.c_str(), "%d:%d", &bh, &bm) != 2) return;
    if (bh < 0 || bh > 23 || bm < 0 || bm > 59) return;
    const int build_min = bh * 60 + bm;
    std::time_t now_tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now_tt);
    // is_us_trading_day, not a bare weekday test. A weekday test runs the whole
    // scanner + tournament against a dead market every Thanksgiving and every
    // Good Friday, and its verdict can page Critical ("lineup: ABORTED" —
    // alert_rules.h), which is the same false-page-on-a-closed-market class as
    // the 2026-08-11 history pages. market_calendar.h was written for exactly
    // this and was only ever wired into the pre-open check; this is the second
    // of its call sites. Doing nothing on a holiday is free — yesterday's
    // lineup simply stands until the next real open.
    if (!is_us_trading_day(tm)) return;
    if (tm.tm_yday == lineup_last_build_day_) return;   // one build per day
    const int now_min = tm.tm_hour * 60 + tm.tm_min;
    if (now_min < build_min || now_min > build_min + 30) return;   // in the window
    lineup_last_build_day_ = tm.tm_yday;
    route("lineup: scheduled build (" + cfg_.lineup_build_time +
             (cfg_.lineup_propose_only ? ", propose-only)" : ", auto-start)"));
    start_daily_lineup(!cfg_.lineup_propose_only);
}

// The scheduled daily TWS refresh (force broker+feed to drop & reconnect at a
// fixed local time, default 02:00) was REMOVED: it caused two multi-hour
// gateway outages (2026-07-30 froze a healthy connection in the reconnect;
// 2026-07-31 it reconnected into a gateway that had failed its overnight
// re-auth and the app-side went silent). IBKR's own overnight reset already
// self-heals (1100/1102), so the forced reconnect only added risk with no
// benefit. The adapters keep request_reconnect() for a future manual button;
// nothing schedules it automatically. See [[tt-tws-reconnect-freeze]].

// Kick off a live "swap onto the new lineup" (see SwapStage). Snapshots the
// running session, works out which symbols are leaving, cancels their resting
// orders (so a bracket leg can't re-open after we close), and market-closes
// their positions. The stop + restart happens in pump_lineup_swap once those
// drops are confirmed flat — symbols that stay are never touched.
void App::begin_lineup_swap(const TradePanel::StartOpts& next) {
    if (swap_stage_ != SwapStage::None) {
        route("lineup swap: a swap is already in progress - skipping");
        return;
    }
    auto upper = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
        return s;
    };
    std::vector<std::string> keep;   // symbols the new lineup will trade
    for (const auto& so : next.symbols) keep.push_back(upper(so.symbol));
    auto stays = [&](const std::string& s) {
        return std::find(keep.begin(), keep.end(), s) != keep.end();
    };
    const LiveSnapshot snap = engine_.live_snapshot();
    // REFUSE TO DROP A SYMBOL THE BROKER SAYS WE ARE STILL IN.
    //
    // The drop test below used to be `snap.symbols[i].position.qty != 0.0`
    // alone — the APP's own book, which is exactly the number this whole class
    // of bug corrupts. On 2026-08-13 that number was a phantom for four hours.
    // On 2026-08-06 a dropped symbol's market-close never filled and the $846
    // position it left behind was invisible to every later session.
    //
    // The moment a symbol leaves cfg_.symbols it stops being adoptable at
    // reconciliation, stops being reachable by flatten(), cancel_all(), the kill
    // switch and the 15:57 EOD backstop, and stops being auditable at all — see
    // engine/reconcile_policy.h, where 20 NVDA shares lived undetected for 24
    // days for precisely this reason. So a symbol the broker's last audit shows
    // non-flat is KEPT in the next lineup instead of being dropped and closed:
    // on the restart, reconciliation adopts the position and adopt_hold pauses
    // its strategy until it goes flat, so nothing trades it while it is held.
    //
    // Keeping, not force-closing on the broker's number: submitting a sell for
    // shares the engine's portfolio does not know about would take the app's
    // book to -20 while the broker went to 0 — inventing a second, opposite
    // divergence out of the first. This detector is compare-only for that
    // reason (net/book_divergence.h); the app must not trade on a number it
    // cannot reconcile.
    std::vector<std::string> forced_keep;
    std::vector<TradePanel::SymbolOpt> forced_keep_opts;
    for (const SymbolState& ss : snap.symbols) {
        if (stays(upper(ss.symbol))) continue;
        bool unknown = false;
        if (!book_audit_.broker_holds(ss.symbol, unknown)) continue;
        forced_keep.push_back(ss.symbol);
        keep.push_back(upper(ss.symbol));
    }
    if (!forced_keep.empty()) {
        // Carried into the RESTART as well, or "keeping it" would last exactly
        // until the new session's cfg.symbols was built without it. Bar size,
        // account and risk are copied off the new lineup's first symbol; the
        // strategy is left empty because this tab exists to hold a position to
        // flat, not to trade.
        std::string list;
        for (const std::string& s : forced_keep) {
            TradePanel::SymbolOpt so;
            if (!next.symbols.empty()) {
                so = next.symbols.front();
                so.params.clear();
                so.inherited_params.clear();
                so.param_source = ParamSource::None;
                so.strat_key.clear();
                // The flag is what actually stops it trading. Clearing the key
                // does NOT leave the tab strategy-less: acquire_strategy maps ""
                // to kBuiltinStrategyKey ("sma_crossover.cpp"), a real promoted
                // strategy, which would then run on its declared defaults and
                // open a position the instant adopt_hold released the symbol -
                // on a symbol the lineup had just decided not to trade.
                so.hold_only = true;
                so.ap_mode = 0;
            }
            so.symbol = s;
            forced_keep_opts.push_back(std::move(so));
            list += (list.empty() ? "" : ", ") + s;
        }
        route_operator(
            "lineup swap: KEEPING " + list +
            " in the session - the broker's book still shows a position there, and a "
            "symbol outside cfg.symbols can be neither adopted, audited, nor "
            "flattened (2026-08-06 orphan, $846; 2026-07-21 NVDA, 24 days). It is "
            "carried HOLD-ONLY: reconciliation adopts it and its exits still "
            "work, but it may not open or add.");
    }
    std::vector<uint32_t> dropped_ids;   // symbols leaving the lineup (any state)
    swap_flatten_ids_.clear();
    for (size_t i = 0; i < snap.symbols.size(); ++i) {
        if (stays(upper(snap.symbols[i].symbol))) continue;   // carried over
        const uint32_t id = static_cast<uint32_t>(i + 1);     // index 0 = symbol_id 1
        dropped_ids.push_back(id);
        if (snap.symbols[i].position.qty != 0.0)
            swap_flatten_ids_.push_back(id);                  // has a position -> close it
    }
    auto is_dropped = [&](uint32_t id) {
        return std::find(dropped_ids.begin(), dropped_ids.end(), id) != dropped_ids.end();
    };
    // Cancel resting orders on the dropped symbols first (engine-direct). Leave
    // the stayers' resting orders alone — they're re-adopted on the restart.
    int cancelled = 0;
    for (const OrderRecord& o : snap.orders)
        if (o.status == OrderStatus::Working && is_dropped(o.symbol_id)) {
            engine_.request_cancel(o.id);
            ++cancelled;
        }
    // Market-close the dropped positions. submit_manual bypasses the strategy
    // order gate, so a losing drop still closes despite hold-until-profitable.
    for (uint32_t id : swap_flatten_ids_) {
        const double q = snap.symbols[id - 1].position.qty;
        engine_.submit_manual(id, q < 0.0, std::fabs(q));
    }
    swap_opts_ = next;
    // ...plus anything the broker says we may not walk away from (above).
    for (TradePanel::SymbolOpt& so : forced_keep_opts)
        swap_opts_.symbols.push_back(std::move(so));
    swap_stage_ = SwapStage::Flatten;
    swap_deadline_s_ = mono_s() + 30.0;   // give the closes 30s to fill
    route("lineup swap: closing " + std::to_string(swap_flatten_ids_.size()) +
          " dropped position(s), cancelled " + std::to_string(cancelled) +
          " resting order(s); keeping the rest");
}

// Drive an in-progress lineup swap. Once the dropped positions are flat, stop
// (keeping the carried-over positions) and restart on the new lineup; on the
// restart, reconciliation re-adopts + holds them. If the closes don't fill
// before the deadline, fall back to a full flatten so nothing is orphaned.
void App::pump_lineup_swap() {
    if (swap_stage_ == SwapStage::None) return;
    if (swap_stage_ == SwapStage::Flatten) {
        const LiveSnapshot snap = engine_.live_snapshot();
        bool all_flat = true;
        for (uint32_t id : swap_flatten_ids_)
            if (id >= 1 && id <= snap.symbols.size() &&
                snap.symbols[id - 1].position.qty != 0.0) {
                all_flat = false;
                break;
            }
        if (!all_flat && mono_s() < swap_deadline_s_) return;   // still closing
        if (!all_flat)
            route("lineup swap: dropped positions didn't flatten in time - stopping "
                  "with a full flatten (kill switch) instead of keeping positions");
        // Keep the carried-over positions only if the drops are confirmed flat.
        safe_stop_live(all_flat);
        swap_flatten_ids_.clear();
        swap_stage_ = SwapStage::Restart;   // restart next frame (stop_live has joined)
        swap_reap_deadline_s_ = mono_s() + kSwapReapWaitS;
        return;
    }
    if (swap_stage_ == SwapStage::Restart) {
        // WAIT FOR OUR OWN SOCKET TO CLOSE. The orders client id is fixed at
        // kTwsOrdersClientId now (see start_live_session), so restarting on the
        // very next frame — ~16 ms after safe_stop_live handed the old adapter
        // to a detached thread — would have the new session ask the gateway for
        // an id this process has not finished releasing. reap_async detaches,
        // and the old I/O thread can sit in waitForSignal for up to a second
        // before it notices stop_ and calls eDisconnect.
        //
        // This waits on a fact we OWN and can observe: the destructor returned.
        // It deliberately does not wait on "the gateway has probably freed it by
        // now", which is unobservable from here, so any duration would be a
        // guess. Past the deadline it proceeds anyway rather than stranding the
        // swap: the 326 tier (fast retries, page only after a minute) is the
        // backstop, and a session that never restarts is worse than one that
        // spends a few seconds retrying.
        if (broker_reaps_->load(std::memory_order_acquire) > 0 &&
            mono_s() < swap_reap_deadline_s_)
            return;   // spin frames; the render loop keeps running
        if (broker_reaps_->load(std::memory_order_acquire) > 0)
            route("lineup swap: previous broker still shutting down after " +
                  std::to_string(static_cast<int>(kSwapReapWaitS)) +
                  "s - restarting anyway (expect a brief client-id retry)");
        swap_stage_ = SwapStage::None;
        if (engine_.live_running()) {   // stop_live joins, so this shouldn't happen
            route("lineup swap: engine still live after stop - aborting restart");
            swap_opts_ = {};
            return;
        }
        route("lineup swap: restarting the live session on the new lineup");
        start_live_session(swap_opts_);
        swap_opts_ = {};
    }
}

// How long the ranking pass may go without a single arrival, and how long the
// whole pass may take. Seconds, on mono_s(). Both replace a flat 15 s
// quiet timer that was SHORTER than the deliberate pacing of the pass's own
// requests, which is what made a paced pass indistinguishable from a failed one.
//
// THE QUIET STRETCH. Thirty requests at kHistMinGapMs (500 ms) occupy 15 s of
// spacing on their own; a straggler that IB silently declines is classified dead
// at kHistTimeoutMs (20 s) and retried once, so its answer can legitimately land
// 40 s after the previous symbol's. 60 s clears that with margin.
static constexpr double kLineupFetchQuietS = 60.0;
// THE HARD DEADLINE, from phase entry. It has to outlast the longest a request
// can legitimately be in the system: kHistQueueMaxWaitMs (90 s) of pacing hold
// plus a fetch's own death-and-retry. Reaching it means the data session is not
// answering, which the log now says explicitly rather than silently ranking
// whatever turned up.
static constexpr double kLineupFetchDeadlineS = 150.0;

void App::start_daily_lineup(bool autostart_when_done) {
    if (lineup_.phase != DailyLineup::Phase::Idle) {
        route("lineup: already building");
        return;
    }
    if (!use_tws_data_) {
        route("lineup: requires the IBKR (TWS) data route");
        return;
    }
    if (!data_.connected()) {
        route("lineup: data session not connected");
        return;
    }
    if (engine_.running()) {
        route("lineup: stop the live session first");
        return;
    }
    if (tourn_.active || sweep_.running) {
        route("lineup: optimizer busy, try again");
        return;
    }
    lineup_ = DailyLineup{};
    lineup_.autostart = autostart_when_done;
    lineup_.spec.scan_code = cfg_.lineup_scan_code;
    lineup_.spec.location = cfg_.lineup_location;
    lineup_.spec.instrument = "STK";
    lineup_.spec.rows = cfg_.lineup_rows > 0 ? cfg_.lineup_rows : 30;
    lineup_.spec.price_above = cfg_.lineup_min_price;
    lineup_.rank.atr_len = cfg_.lineup_atr_len > 0 ? cfg_.lineup_atr_len : 14;
    lineup_.rank.min_price = cfg_.lineup_min_price;
    lineup_.rank.min_dollar_vol = cfg_.lineup_min_dollar_vol;
    lineup_.rank.top_n = cfg_.lineup_top_n > 0
                             ? static_cast<std::size_t>(cfg_.lineup_top_n)
                             : 6;
    {
        std::lock_guard<std::mutex> g(lineup_mu_);
        lineup_hits_.clear();
        lineup_hits_ready_ = false;
        lineup_want_bars_.clear();
        lineup_bar_inbox_.clear();
    }
    tws_data_.request_scan(lineup_.spec, [this](std::vector<net::ScanHit> hits) {
        std::lock_guard<std::mutex> g(lineup_mu_);
        lineup_hits_ = std::move(hits);
        lineup_hits_ready_ = true;
    });
    lineup_.phase = DailyLineup::Phase::Scanning;
    lineup_.stamp_s = mono_s();
    route("lineup: scanning IBKR for high-volatility movers...");
}

// I/O thread (on_candles): stage a pool symbol's daily bars for the UI thread.
void App::collect_lineup_bars(net::CandleBatch& b) {
    std::lock_guard<std::mutex> g(lineup_mu_);
    // By request id: this batch is the ranking pass's only if the ranking pass
    // asked for it. See lineup_want_bars_.
    const auto it = lineup_want_bars_.find(b.id);
    if (it == lineup_want_bars_.end()) return;
    if (it->second != b.symbol) return;   // id space corrupted; never rank on it
    lineup_want_bars_.erase(it);          // one delivery per request
    std::vector<tt::RankBar> bars;
    bars.reserve(b.candles.size());
    for (const auto& c : b.candles)
        bars.push_back({c.high, c.low, c.close, c.volume});
    lineup_bar_inbox_.emplace_back(b.symbol, std::move(bars));
}

namespace {
// Is the account still in this symbol — a position, or a working order that can
// open one?
//
// Exclusion deletes a Trade tab, and for an exposed symbol that is not a refusal
// to trade, it is a decision ABOUT the position: with a session running,
// begin_lineup_swap flattens every dropped symbol with submit_manual (which
// deliberately bypasses the strategy's hold-until-profitable gate, so a
// transient candle-fetch timeout would realize a real loss); with no session
// running, the symbol is absent from cfg.symbols, so reconciliation's sid_for()
// returns 0, the position is never adopted, never appears in /diag, and is
// invisible to both pump_orphan_watchdog and the 15:57 EOD backstop.
//
// The snapshot survives stop_live (only `running` is cleared), so the usual
// pre-open case — the previous session ended holding something for the
// hold-until-flat restart — is covered. After an app restart the snapshot is
// empty and this reports no exposure; that is a real blind spot, but it is the
// pre-0.16.0 behaviour rather than a regression, and reconciliation is what
// covers it once the symbol is in the session.
bool symbol_has_exposure(const LiveSnapshot& s, const std::string& symbol) {
    auto upper = [](std::string x) {
        for (char& c : x) c = static_cast<char>(std::toupper((unsigned char)c));
        return x;
    };
    const std::string want = upper(symbol);
    for (size_t i = 0; i < s.symbols.size(); ++i) {
        if (upper(s.symbols[i].symbol) != want) continue;
        if (s.symbols[i].position.qty != 0.0) return true;
        const uint32_t sid = static_cast<uint32_t>(i + 1);
        for (const OrderRecord& o : s.orders)
            if (o.symbol_id == sid && o.status == OrderStatus::Working) return true;
        return false;
    }
    return false;
}
} // namespace

void App::pump_daily_lineup() {
    using Phase = DailyLineup::Phase;
    const double now = mono_s();
    switch (lineup_.phase) {
    case Phase::Idle:
        return;

    case Phase::Scanning: {
        std::vector<net::ScanHit> hits;
        bool ready = false;
        {
            std::lock_guard<std::mutex> g(lineup_mu_);
            if (lineup_hits_ready_) {
                hits = std::move(lineup_hits_);
                lineup_hits_.clear();
                lineup_hits_ready_ = false;
                ready = true;
            }
        }
        if (!ready) {
            if (now - lineup_.stamp_s > 30.0) {
                route("lineup: scan timed out");
                lineup_.phase = Phase::Idle;
            }
            return;
        }
        std::set<std::string> seen;
        for (const auto& h : hits) {
            if (h.symbol.empty() || seen.count(h.symbol)) continue;
            seen.insert(h.symbol);
            lineup_.pool.push_back(h.symbol);
        }
        if (lineup_.pool.empty()) {
            route("lineup: scan returned no candidates");
            lineup_.phase = Phase::Idle;
            return;
        }
        route("lineup: " + std::to_string(lineup_.pool.size()) +
                 " candidates - fetching daily bars to rank");
        lineup_.awaiting = {lineup_.pool.begin(), lineup_.pool.end()};
        lineup_.bars.clear();
        // Under the lock across every request_candles call, so no delivery can
        // arrive before the id that claims it is on record. This is the pass that
        // put 30 requests on the wire in one second on 2026-08-10; they are still
        // issued in one go, and net/hist_pacing.h is what now spreads them.
        {
            std::lock_guard<std::mutex> g(lineup_mu_);
            lineup_want_bars_.clear();
            lineup_bar_inbox_.clear();
            lineup_dropped_.clear();
            for (const std::string& sym : lineup_.pool) {
                const uint32_t rid = data_.request_candles(sym, "1d", "1mo");
                if (rid) lineup_want_bars_[rid] = sym;
                else lineup_.awaiting.erase(sym);   // never asked; never wait for it
            }
        }
        lineup_.phase = Phase::FetchingBars;
        lineup_.stamp_s = lineup_.fetch_start_s = now;
        return;
    }

    case Phase::FetchingBars: {
        std::vector<std::pair<std::string, std::vector<tt::RankBar>>> arrived;
        std::vector<std::string> dropped;
        {
            std::lock_guard<std::mutex> g(lineup_mu_);
            arrived.swap(lineup_bar_inbox_);
            dropped.swap(lineup_dropped_);
        }
        for (auto& [sym, bars] : arrived) {
            lineup_.bars[sym] = std::move(bars);
            lineup_.awaiting.erase(sym);
        }
        // A request the feed errored out is ANSWERED, just not with bars. Before
        // this the only way to notice was the quiet timer below, which cannot
        // tell a failure from a request that is merely being paced.
        for (const std::string& sym : dropped) lineup_.awaiting.erase(sym);
        if (!arrived.empty() || !dropped.empty())
            lineup_.stamp_s = now;                     // reset the quiet timer
        // Proceed once every symbol has answered one way or the other. The two
        // backstops behind that are both LONGER than the pacing that legitimately
        // delays this pass — see the constants. The old 15 s quiet timer was
        // shorter than the deliberate spacing of the pass's own 30 requests, so a
        // held ranking pass advanced with the pool unfetched and the day's
        // symbols were chosen from whichever few had been sent.
        const bool quiet = now - lineup_.stamp_s > kLineupFetchQuietS;
        const bool expired = now - lineup_.fetch_start_s > kLineupFetchDeadlineS;
        if (!lineup_.awaiting.empty() && !quiet && !expired) return;
        {
            std::lock_guard<std::mutex> g(lineup_mu_);
            lineup_want_bars_.clear();
        }
        // Say it when the pool is short. Ranking a partial pool is a legitimate
        // outcome — one dead ticker must not cost the whole day — but it is NOT
        // the same build as a complete one, and nothing in the log used to
        // distinguish "the 6 most volatile of 30" from "the 6 that answered".
        if (!lineup_.awaiting.empty())
            route("lineup: WARNING ranking a PARTIAL pool - " +
                     std::to_string(lineup_.bars.size()) + " of " +
                     std::to_string(lineup_.pool.size()) +
                     " symbols delivered bars (" +
                     (expired ? "hit the " +
                                    std::to_string(static_cast<int>(kLineupFetchDeadlineS)) +
                                    "s fetch deadline"
                              : "no arrivals for " +
                                    std::to_string(static_cast<int>(kLineupFetchQuietS)) +
                                    "s") +
                     ") - today's picks come from that subset, not the full scan");
        lineup_.phase = Phase::Ranking;
        lineup_.stamp_s = now;
        return;
    }

    case Phase::Ranking: {
        std::vector<tt::RankCandidate> cands;
        cands.reserve(lineup_.bars.size());
        for (auto& [sym, bars] : lineup_.bars)
            cands.push_back({sym, std::move(bars)});
        const auto ranked = tt::rank_by_volatility(cands, lineup_.rank);
        lineup_.picks.clear();
        for (const auto& r : ranked) lineup_.picks.push_back(r.symbol);
        if (lineup_.picks.empty()) {
            route("lineup: no candidate cleared the price/liquidity gates");
            lineup_.phase = Phase::Idle;
            return;
        }
        std::string msg = "lineup: picks -";
        for (const auto& r : ranked) {
            char b[48];
            std::snprintf(b, sizeof b, " %s(%.1f%%)", r.symbol.c_str(),
                          r.atr_pct * 100.0);
            msg += b;
        }
        route(msg);
        trade_.set_lineup(lineup_.picks);
        show_trade_ = true;   // surface the freshly built tabs
        lineup_.fitted.clear();
        lineup_.tourn_idx = 0;
        lineup_.phase = Phase::Tournaments;
        lineup_.stamp_s = now;
        return;
    }

    case Phase::Tournaments: {
        if (tourn_.active || sweep_.running) return;   // let the current one finish
        // Past that guard, any tournament this build launched has ENDED — this
        // is the only point in the frame where that is true, and tourn_.results
        // is still the field that just ran (start_tournament clears it). A dry
        // run has to read it here or not at all. No-op unless one is active.
        dryrun_tournament_settled();
        if (lineup_.tourn_idx >= lineup_.picks.size()) {
            lineup_.phase = Phase::Done;
            return;
        }
        const SweepPanel::Settings st = sweep_panel_.settings();
        static constexpr const char* kRng[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};
        const std::string sym = lineup_.picks[lineup_.tourn_idx++];
        SweepPanel::Request rq;
        rq.symbol = sym;
        rq.interval = sweep_interval_str(st.interval_idx);
        rq.range = kRng[std::clamp(st.range_idx, 0, 5)];
        rq.cash = st.cash;
        rq.metric = st.metric;
        rq.holdout_pct = st.holdout ? st.holdout_pct : 25.0;
        route("lineup: tournament " + std::to_string(lineup_.tourn_idx) + "/" +
                 std::to_string(lineup_.picks.size()) + " - " + sym);
        start_tournament(rq, sym, {}, /*for_lineup=*/true);
        // Stamp the pick even if start_tournament refused: a refusal is one of
        // the ways a pick ends the build with no fit, and a dry run that only
        // timed the tournaments that STARTED would report a shorter build with
        // fewer symbols than it actually had.
        if (dry_.running) {
            dry_.tourn_symbol = sym;
            dry_.tourn_start_ms = mono_ms();
        }
        return;
    }

    case Phase::Done: {
        // Admission. A pick whose tournament installed no fit on its own tab has
        // no parameters fitted to itself; before 0.16.0 it traded anyway on
        // whatever the shared per-strategy map happened to hold, which on
        // 2026-08-10 meant four symbols running an SSPC fit. Decide with
        // plan_lineup (symbol_params.h) so the rule is testable.
        const LiveSnapshot lsnap = engine_.live_snapshot();
        std::vector<std::pair<std::string, SymbolOutcome>> outcomes;
        for (const std::string& sym : trade_.pending_symbols()) {
            SymbolOutcome o;
            o.tournament_ran =
                std::find(lineup_.picks.begin(), lineup_.picks.end(), sym) !=
                lineup_.picks.end();
            // Positive test: only finish_tournament's install line fills this
            // set. Every abort, timeout and rejection therefore reads as "no
            // fit", which is the safe direction.
            o.fitted_this_build = lineup_.fitted.count(sym) != 0;
            o.has_own_params = trade_.has_own_params(sym, param_specs_fn());
            o.holds_position = symbol_has_exposure(lsnap, sym);
            // Can the set this tab would trade close a losing position before
            // the 15:57 flatten? 2026-08-14: STKH went live on a bollinger_
            // reversion fit with time_stop = 233 bars of 300 s — three trading
            // days — and BollRev places no price stop, so for 2 h 16 m nothing
            // in the system could exit that position at a loss. -$506, 85% of
            // the day. See tt::ui::time_stop_reachable.
            const TradePanel::TabParams tp = trade_.tab_params(sym, param_specs_fn());
            o.time_stop_reachable =
                !tp.found ||
                time_stop_reachable(tp.params, tp.bar_seconds, tp.hold_dont_halt);
            outcomes.emplace_back(sym, o);
        }
        const LineupPlan plan = plan_lineup(outcomes);
        // The dry run's per-symbol verdicts come from THIS plan, not from a
        // second reading of the same inputs. No-op unless one is active.
        dryrun_record_plan(plan);
        for (const std::string& sym : plan.own_previous)
            route_operator("lineup: " + sym +
                     " has no fit from this morning's tournament - trading its OWN "
                     "previous parameters (fitted to " + sym + ", just not today)");
        for (const std::string& sym : plan.holding_only)
            route_operator("lineup: " + sym +
                     " has NO parameters of its own but the account is still in it - "
                     "kept in the session so the position is adopted and held until "
                     "flat, NOT dropped (dropping it would market-close the position "
                     "or orphan it entirely). It will inherit the shared defaults if "
                     "it goes flat - check it.");
        if (!plan.start) {
            // Every pick failed. A session where every symbol runs borrowed
            // parameters is worse than no session, so start nothing and say so.
            //
            // "Say so" is not enough on its own: the Trade panel's scheduled
            // auto-start is level-triggered and knows nothing about the lineup,
            // so tabs merely "left in place" would have been started by it a
            // frame later on exactly the borrowed parameters this refused. Block
            // it for the day; the operator can still press Start by hand.
            route_operator("lineup: ABORTED - not one of " +
                     std::to_string(lineup_.picks.size()) +
                     " picks produced a usable parameter set. Starting NOTHING "
                     "rather than trading on another symbol's parameters; the "
                     "scheduled auto-start is blocked for today. Tabs left in place "
                     "for inspection.");
            trade_.block_scheduled_start();
            lineup_autostart_pending_ = false;
            lineup_.phase = Phase::Idle;
            return;
        }
        if (!plan.unreachable_stop.empty()) {
            // Its OWN line, and it is not the no-fit line. These symbols DO have
            // a parameter set fitted to them; the set is unrealizable. Folding
            // that into "no fit from this morning's tournament" would send the
            // operator looking for a tournament failure that did not happen and
            // hide the one thing worth acting on — the optimizer is still
            // proposing holds the trading day cannot contain.
            std::string list;
            for (const std::string& s : plan.unreachable_stop)
                list += (list.empty() ? "" : ", ") + s;
            route_operator(
                "lineup: EXCLUDED " + list +
                " - their time stop cannot fire inside a trading day (" +
                std::to_string(session_bars(cfg_.trade_bar_sec)) +
                " bars from the open to the 15:57 flatten at this bar size). These "
                "strategies place NO price stop, so the time stop is the only thing "
                "that ever closes a losing position - 2026-08-14 STKH held 233 bars' "
                "worth of nothing and lost $506. Refit them; do not hand-edit the "
                "number, a value no backtest scored is not a fit.");
        }
        {
            // The no-fit line names only the no-fit symbols. plan.excluded is
            // the whole set that will not trade (both faults), because that is
            // what the tabs have to be removed by.
            std::string list;
            for (const std::string& s : plan.excluded) {
                if (std::find(plan.unreachable_stop.begin(),
                              plan.unreachable_stop.end(),
                              s) != plan.unreachable_stop.end())
                    continue;   // already reported above, with its own reason
                list += (list.empty() ? "" : ", ") + s;
            }
            if (!list.empty())
                route_operator("lineup: EXCLUDED " + list +
                     " - no fit from this morning's tournament and no parameters of "
                     "their own; trading them would mean running another symbol's "
                     "fit. Nothing is held in them, so dropping the tab is safe.");
            if (!plan.excluded.empty()) trade_.remove_symbols(plan.excluded);
        }
        route("lineup: ready - " + std::to_string(plan.admitted.size()) +
                 " symbols loaded into the Trade tabs");
        // Arm the auto-start for the draw() call site, which has the account +
        // data-availability context to build the StartOpts (see start_live_session
        // wiring). propose_only lineups leave this false and just log the picks.
        if (lineup_.autostart) lineup_autostart_pending_ = true;
        lineup_.phase = Phase::Idle;
        return;
    }
    }
}

// ---- headless daily-lineup dry run (TT_AUTORUN_LINEUP=1) ------------------
//
// See App::LineupDryRun. This is instrumentation around the production lineup,
// not a second implementation of it: the only thing here that CAUSES anything is
// the one start_daily_lineup() call below, which is the same call the 09:35
// schedule makes. Everything else reads state the build has already produced.

// Phase names for the "dryrun: phase=" lines. Short, stable, no spaces —
// scripts group on these.
const char* App::dryrun_phase_name(DailyLineup::Phase p) {
    switch (p) {
    case DailyLineup::Phase::Scanning:     return "scan";
    case DailyLineup::Phase::FetchingBars: return "fetch_bars";
    case DailyLineup::Phase::Ranking:      return "rank";
    case DailyLineup::Phase::Tournaments:  return "tournaments";
    case DailyLineup::Phase::Done:         return "admit";
    case DailyLineup::Phase::Idle:         break;
    }
    return "idle";
}

// How long the dry run waits for a data session before giving up. The app
// launches IB Gateway itself on the TWS route and a cold login is minutes, so
// this has to be generous — but it has to EXIST: without it a box whose gateway
// never comes up leaves the process sitting at a hidden window forever, and the
// only thing that would ever notice is the caller's kill timeout, which cannot
// say why. This exits non-zero with a reason instead.
static constexpr int64_t kDryRunConnectWaitMs = 5 * 60 * 1000;

void App::pump_lineup_dryrun() {
    if (!dry_.active) return;
    using Phase = DailyLineup::Phase;
    const int64_t now = mono_ms();

    // ---- trigger: once, as soon as the data session can serve a scan --------
    if (!dry_.triggered) {
        if (!data_.connected()) {
            if (now > kDryRunConnectWaitMs) {
                dry_.triggered = dry_.running = true;   // so finish will report
                dry_.start_ms = 0;
                route_operator(dryrun_start_line());
                dryrun_finish("no-data-session");
            }
            return;
        }
        dry_.triggered = true;
        dry_.running = true;
        dry_.start_ms = dry_.phase_ms = now;
        dry_.hist0 = data_.hist_stats();   // counters are process-lifetime; bracket them
        route_operator(dryrun_start_line());
        // autostart_when_done = FALSE, hard-coded. Not cfg_.lineup_propose_only:
        // the persisted config is exactly what a dry run must not be able to
        // trade on. start_live_session() refuses for the whole run as well, so
        // this is belt and braces rather than the only guard.
        start_daily_lineup(/*autostart_when_done=*/false);
        if (lineup_.phase == Phase::Idle) {
            // Refused before it began (wrong data route, session not up,
            // optimizer busy). start_daily_lineup already logged which.
            dryrun_finish("not-started");
            return;
        }
        dry_.phase = lineup_.phase;
        dry_.phase_ms = now;
        return;
    }
    if (!dry_.running) return;

    // ---- observe: one line per phase, as the phase ENDS --------------------
    if (lineup_.phase == dry_.phase) return;
    std::string detail;
    switch (dry_.phase) {
    case Phase::Scanning:
        dry_.sum.pool = lineup_.pool.size();
        detail = "pool=" + std::to_string(lineup_.pool.size());
        break;
    case Phase::FetchingBars:
        dry_.sum.delivered = lineup_.bars.size();
        detail = "delivered=" + std::to_string(lineup_.bars.size()) + '/' +
                 std::to_string(lineup_.pool.size()) +
                 " awaiting=" + std::to_string(lineup_.awaiting.size());
        break;
    case Phase::Ranking:
        dry_.sum.picks = lineup_.picks.size();
        detail = "picks=" + std::to_string(lineup_.picks.size());
        break;
    case Phase::Tournaments:
        detail = "symbols=" + std::to_string(dry_.syms.size());
        break;
    case Phase::Done:
        detail = "fitted=" +
                 std::to_string(dryrun_count(dry_.syms, DryRunOutcome::Fitted));
        break;
    case Phase::Idle:
        break;   // never the phase that "ended"
    }
    route_operator(dryrun_phase_line(dryrun_phase_name(dry_.phase),
                                     now - dry_.phase_ms, detail));
    const Phase ended = dry_.phase;
    dry_.phase = lineup_.phase;
    dry_.phase_ms = now;
    if (lineup_.phase != Phase::Idle) return;
    // Back to Idle = the build settled. Reaching Idle from anywhere but Done is
    // one of the lineup's early exits, and the phase it died in is the one thing
    // that tells them apart; the lineup logs its own reason in prose, this makes
    // it a token. Reaching it from Done is a build that ran to completion —
    // which is NOT the same as a build that produced something, so admission's
    // own refusal gets a token of its own.
    dryrun_finish(ended != Phase::Done
                      ? std::string("gave-up-in-") + dryrun_phase_name(ended)
                      : (dry_.admission_refused ? std::string("admission-refused")
                                                : std::string()));
}

// A pick's tournament has ended (or never started). Called from
// pump_daily_lineup at the one point in the frame where the field that just ran
// is still readable.
void App::dryrun_tournament_settled() {
    if (!dry_.running || dry_.tourn_symbol.empty()) return;
    DryRunSymbol s;
    s.symbol = dry_.tourn_symbol;
    s.tournament_ms = mono_ms() - dry_.tourn_start_ms;
    // Only read the tournament if it is still OURS. Nothing else should be able
    // to run one here (the autopilot is gated on a live session, and a dry run
    // has none), but attributing another target's field to this pick would be a
    // silent lie of exactly the kind Tournament::for_lineup exists to prevent.
    if (tourn_.target_symbol == s.symbol) {
        s.candidates_total = static_cast<int>(tourn_.candidates.size());
        for (const Tournament::Entry& e : tourn_.results)
            if (e.valid) ++s.candidates_ok;
    }
    // Provisional: what the TOURNAMENT achieved. Admission has not run yet, so
    // a symbol with no fit today may still turn out to be admitted on its own
    // earlier fit or excluded — dryrun_record_plan refines this, and the summary
    // is what carries the final verdict.
    s.outcome = lineup_.fitted.count(s.symbol) ? DryRunOutcome::Fitted
                                               : DryRunOutcome::NoCandidate;
    dry_.syms.push_back(s);
    route_operator(dryrun_tournament_line(s, dry_.syms.size(),
                                          lineup_.picks.size()));
    dry_.tourn_symbol.clear();
}

// Admission decided. Copy its verdicts onto the picks we timed — the plan is the
// production rule (symbol_params.h), and re-deriving it here would let the
// report drift from what the build actually did.
void App::dryrun_record_plan(const LineupPlan& plan) {
    if (!dry_.running) return;
    auto mark = [&](const std::vector<std::string>& list, DryRunOutcome o) {
        for (const std::string& sym : list)
            for (DryRunSymbol& s : dry_.syms)
                // Fitted is this build's own achievement and outranks every
                // admission verdict: a fitted symbol appears in plan.admitted
                // and must not be relabelled by it.
                if (s.symbol == sym && s.outcome != DryRunOutcome::Fitted)
                    s.outcome = o;
    };
    mark(plan.own_previous, DryRunOutcome::OwnPrevious);
    mark(plan.holding_only, DryRunOutcome::HoldingOnly);
    mark(plan.excluded, DryRunOutcome::Excluded);
    dry_.admission_refused = !plan.start;
}

// The build settled: emit the one line a monitor keeps, set the process status,
// and quit. `abort` is "" for a build that ran through to admission.
void App::dryrun_finish(const std::string& abort) {
    if (!dry_.running) return;
    dry_.running = false;
    dry_.sum.total_ms = mono_ms() - dry_.start_ms;
    dry_.sum.abort = abort;
    // The DELTA across the build. The source's counters are process-lifetime and
    // a chart or warmup fetch spends them too, so reporting the totals would
    // charge this build for requests it never made.
    const net::HistStats h1 = data_.hist_stats();
    auto delta = [](uint64_t now, uint64_t then) { return now >= then ? now - then : 0; };
    dry_.sum.hist.cache_served = delta(h1.cache_served, dry_.hist0.cache_served);
    dry_.sum.hist.cache_fetched = delta(h1.cache_fetched, dry_.hist0.cache_fetched);
    dry_.sum.hist.cache_lookups = delta(h1.cache_lookups, dry_.hist0.cache_lookups);
    dry_.sum.hist.requests_sent = delta(h1.requests_sent, dry_.hist0.requests_sent);
    dry_.sum.hist.held_min_gap = delta(h1.held_min_gap, dry_.hist0.held_min_gap);
    dry_.sum.hist.held_identical = delta(h1.held_identical, dry_.hist0.held_identical);
    dry_.sum.hist.held_budget = delta(h1.held_budget, dry_.hist0.held_budget);
    dry_.sum.hist.abandoned = delta(h1.abandoned, dry_.hist0.abandoned);
    route_operator(dryrun_summary_line(dry_.sum, dry_.syms));
    exit_code_ = dryrun_exit_code(dry_.syms);
    // request_quit(), not should_quit_ = true: it is the path that refuses to
    // quit out from under a live session. Nothing should be live here — the
    // start guard above makes sure of it — but the dry run is not the place to
    // invent a second way out of the app.
    request_quit();
}

namespace {
// Challenger must beat the incumbent by a real margin — 5% of the incumbent's
// scale (floored so near-zero incumbents don't make any noise a "win").
bool ap_better(int metric, double challenger, double incumbent) {
    const double margin = 0.05 * std::max(std::abs(incumbent), 0.1);
    return tt::ui::sweep_metric_minimize(metric) ? challenger < incumbent - margin
                                                 : challenger > incumbent + margin;
}
} // namespace

void App::pump_autopilot() {
    if (!engine_.live_running()) {
        if (!ap_.syms.empty()) ap_ = Autopilot{};   // session over: disarm
        return;
    }
    if (ap_.syms.empty()) return;
    const double now = mono_s();

    // Session-equity high for the drawdown trigger (session-level proxy until
    // per-symbol portfolios exist).
    const LiveSnapshot s = engine_.live_snapshot();
    ap_.session_high_eq = std::max(ap_.session_high_eq, s.equity);
    const double dd = ap_.session_high_eq > 0
                          ? (ap_.session_high_eq - s.equity) / ap_.session_high_eq * 100.0
                          : 0.0;

    // A cycle is in flight: wait for its tournament, then evaluate.
    if (ap_.in_flight >= 0) {
        if (tourn_.active) return;
        autopilot_evaluate();
        ap_.in_flight = -1;
        return;
    }

    // Idle: launch the next due cycle — one at a time, never over a manual run.
    if (tourn_.active || sweep_.running || engine_.running()) return;
    for (int i = 0; i < static_cast<int>(ap_.syms.size()); ++i) {
        Autopilot::Sym& S = ap_.syms[static_cast<size_t>(i)];
        if (S.mode <= 0) continue;
        const bool timer_due = (S.trigger == 0 || S.trigger == 2) &&
                               now - S.last_cycle_s >= S.interval_min * 60.0;
        const bool dd_due = (S.trigger == 1 || S.trigger == 2) && dd >= S.dd_pct &&
                            now - ap_.last_dd_cycle_s >= 600.0;   // 10 min cooldown
        if (!timer_due && !dd_due) continue;

        // Data settings come from the Optimizer panel; symbol from the session.
        const SweepPanel::Settings st = sweep_panel_.settings();
        static constexpr const char* kRng[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};
        SweepPanel::Request rq;
        rq.symbol = S.symbol;
        rq.interval = sweep_interval_str(st.interval_idx);
        rq.range = kRng[std::clamp(st.range_idx, 0, 5)];
        rq.cash = st.cash;
        rq.metric = st.metric;
        rq.holdout_pct = st.holdout ? st.holdout_pct : 25.0;
        ap_.metric = rq.metric;
        std::vector<std::string> candidates;
        if (S.mode == 1) candidates.push_back(S.key);   // params-only: incumbent
        start_tournament(std::move(rq), S.symbol, std::move(candidates));
        if (!tourn_.active) return;   // could not start; retry next frame
        ap_.in_flight = i;
        S.last_cycle_s = now;
        if (dd_due) ap_.last_dd_cycle_s = now;
        route("autopilot: " + S.symbol + " cycle started (" +
                 (S.mode == 1 ? "params" : "full") +
                 (dd_due ? ", drawdown trigger)" : ", timer)"));
        return;
    }
}

// The in-flight cycle's tournament finished: apply its champion to the LIVE
// session under the hysteresis / streak rules.
void App::autopilot_evaluate() {
    Autopilot::Sym& S = ap_.syms[static_cast<size_t>(ap_.in_flight)];
    const bool minimize = sweep_metric_minimize(ap_.metric);
    const Tournament::Entry* champ = nullptr;
    for (const auto& e : tourn_.results)
        if (e.valid &&
            (!champ ||
             (minimize ? e.score < champ->score : e.score > champ->score)))
            champ = &e;
    if (!champ) {
        route("autopilot: " + S.symbol + " cycle produced no result");
        return;
    }
    // A better SCORE is not the whole test, and this gate sits above BOTH
    // install paths (params refresh and strategy swap) because both hand
    // champ->params to a LIVE strategy. The autopilot re-optimizes every 30
    // minutes on the same tournament machinery that crowned STKH's 233-bar time
    // stop on 2026-08-14 — a 19.4-hour hold on a 300 s series, against a day the
    // engine force-flattens at 15:57 — so it is a third route by which an
    // unrealizable fit reaches a running strategy, alongside the daily lineup
    // and the Start button. bollinger_reversion and rsi2_pullback place no price
    // stop, so that time stop is the ONLY thing that would ever close a losing
    // position. Keep the incumbent: it is at least a set the session was allowed
    // to start on. See tt::ui::time_stop_reachable.
    if (!time_stop_reachable(champ->params, cfg_.trade_bar_sec)) {
        const auto it = champ->params.find("time_stop");
        route_operator("autopilot: " + S.symbol + " champion REJECTED - " +
                       kUnreachableStopTag + " (" +
                       std::to_string(static_cast<int>(it != champ->params.end()
                                                           ? it->second : 0.0)) +
                       " bars, only " +
                       std::to_string(session_bars(cfg_.trade_bar_sec)) +
                       " fit in a trading day at this bar size). Keeping the "
                       "incumbent; the fit is unrealizable, not merely worse.");
        S.challenger.clear();
        S.streak = 0;
        return;
    }

    if (champ->key == S.key) {
        // Same strategy: refresh params only if they genuinely score better.
        // The !has_score arm is the FIRST cycle after arming, where there is no
        // incumbent score to beat — it used to apply whatever came back, however
        // bad. Require a positive score there too, or one poor first cycle
        // installs a losing set for the rest of the day.
        const bool first_cycle_ok =
            !S.has_score && (sweep_metric_minimize(ap_.metric) || champ->score > 0.0);
        if (first_cycle_ok || (S.has_score &&
                               ap_better(ap_.metric, champ->score, S.incumbent_score))) {
            // Re-seed: the re-init this queues wipes the strategy's bar history.
            engine_.update_symbol_params(S.sid, champ->params,
                                         seed_bars(S.symbol, cfg_.trade_bar_sec));
            trade_.set_symbol_strategy(S.symbol, champ->key, champ->params);
            // /diag's params_source sits next to the engine's live params, and
            // this line just replaced them with a fit on THIS symbol. Latching
            // the label only at session start left /diag pairing the new values
            // with 09:30's verdict for the rest of the day.
            live_param_source_[S.symbol] = param_source_name(ParamSource::Own);
            S.incumbent_score = champ->score;
            S.has_score = true;
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "autopilot: %s params queued (%s %.4g, applies when flat)",
                          S.symbol.c_str(), kSweepMetrics[ap_.metric], champ->score);
            route(buf);
        } else {
            route("autopilot: " + S.symbol + " kept (no improvement)");
        }
        S.challenger.clear();
        S.streak = 0;
        return;
    }

    // A different strategy won (full mode): swap only after it beats the
    // incumbent decisively twice in a row.
    if (S.has_score && !ap_better(ap_.metric, champ->score, S.incumbent_score)) {
        route("autopilot: " + S.symbol + " challenger " +
                 strat_mgr_.display_name(champ->key) + " not decisive, kept " +
                 strat_mgr_.display_name(S.key));
        S.challenger.clear();
        S.streak = 0;
        return;
    }
    if (S.challenger != champ->key) {
        S.challenger = champ->key;
        S.streak = 1;
        route("autopilot: " + S.symbol + " challenger " +
                 strat_mgr_.display_name(champ->key) + " (win 1/2)");
        return;
    }
    IStrategy* inst = acquire_strategy(champ->key);
    if (!inst) {
        route("autopilot: " + S.symbol + " swap failed (strategy not loaded)");
        return;
    }
    leases_.push_back({inst, champ->key, StrategyLease::Live});
    engine_.swap_symbol_strategy(S.sid, inst, champ->params,
                                 seed_bars(S.symbol, cfg_.trade_bar_sec));
    trade_.set_symbol_strategy(S.symbol, champ->key, champ->params);
    live_param_source_[S.symbol] = param_source_name(ParamSource::Own);   // see above
    S.key = champ->key;
    S.incumbent_score = champ->score;
    S.has_score = true;
    S.challenger.clear();
    S.streak = 0;
    route("autopilot: " + S.symbol + " strategy swap queued -> " +
             strat_mgr_.display_name(champ->key) + " (applies when flat)");
}

App::~App() {
    // Persist settings first: the shutdown watchdog (main.cpp) may hard-kill this
    // process if a network I/O thread wedges during teardown, so config.json must
    // be written before any of the potentially-slow stops below.
    save_config();
    // Stop the diagnostics server next: its provider lambda captures `this` and
    // reads diag_json_, so the accept thread must be joined before any member it
    // could touch is destroyed.
    diag_srv_.stop();
#ifdef _WIN32
    // Tear down the CP gateway + auto-login daemon we started (tied to app
    // life). TWS route: IB Gateway is deliberately left running - its login
    // survives app restarts and nothing else fights over the session. Short wait:
    // the script's cleanup continues in its own process; we don't block exit on
    // it (a stalled logout must not keep us holding the single-instance mutex).
    if (!use_tws_data_) {
        const std::string args = ps_args("Stop-IbkrSession.ps1", true);
        if (!args.empty()) run_hidden(args, /*wait_ms=*/3000);
    }
#endif
    gw_.stop();
    tws_data_.stop();
    if (signin_.worker.joinable()) signin_.worker.join();
}

// Gather panel state into cfg_ and write config.json. Called on exit and once
// a minute from draw() — settings must survive a force-killed exe (rebuilds
// kill the process, and the destructor never runs).
void App::save_config() {
    cfg_.watchlist = watchlist_.symbols();
    cfg_.chart_symbol = chart_.symbol();
    cfg_.chart_interval_idx = chart_.interval_idx();
    cfg_.chart_range_idx = chart_.range_idx();
    cfg_.backtest_cash = backtest_.cash();
    cfg_.trade_cash = trade_.cash();
    cfg_.trade_bar_sec = trade_.bar_sec();
    cfg_.trade_data_idx = trade_.data_idx();
    cfg_.trade_record = trade_.record();
    cfg_.trade_route = trade_.route();
    cfg_.trade_sched_on = trade_.sched_on();
    cfg_.trade_sched_start = trade_.sched_start();
    cfg_.trade_sched_stop = trade_.sched_stop();
    cfg_.trade_sched_blocked_day = trade_.sched_blocked_day();
    cfg_.risk_max_order_qty = trade_.risk().max_order_qty;
    cfg_.risk_max_position_qty = trade_.risk().max_position_qty;
    cfg_.risk_daily_max_loss = trade_.risk().daily_max_loss;
    cfg_.risk_max_drawdown_pct = trade_.risk_dd_pct();
    cfg_.risk_stale_feed_sec = trade_.risk().stale_feed_sec;
    cfg_.trade_symbols = trade_.symbols_config();
    cfg_.backtest_strategy = backtest_.strategy();
    cfg_.replay_strategy = replay_.strategy();
    cfg_.replay_cash = replay_.cash();
    cfg_.replay_bar_sec = replay_.bar_sec();
    {
        const SweepPanel::Settings s = sweep_panel_.settings();
        cfg_.sweep_strategy = s.strat_key;
        cfg_.sweep_symbol = s.symbol;
        cfg_.sweep_interval_idx = s.interval_idx;
        cfg_.sweep_range_idx = s.range_idx;
        cfg_.sweep_cash = s.cash;
        cfg_.sweep_metric = s.metric;
        cfg_.sweep_holdout = s.holdout;
        cfg_.sweep_holdout_pct = s.holdout_pct;
    }
    cfg_.panels = {{"chart", show_chart_},
                   {"watchlist", show_watchlist_},
                   {"backtest", show_backtest_},
                   {"replay", show_replay_},
                   {"optimizer", show_sweep_},
                   {"strategy", show_strategy_},
                   {"build_output", show_build_output_},
                   {"trade", show_trade_},
                   {"blotter", show_blotter_},
                   {"positions", show_positions_},
                   {"journal", show_journal_},
                   {"log", show_log_},
                   {"optlog", show_opt_log_}};
    cfg_.strategy_loaded = strat_mgr_.loaded_keys();
    cfg_.strategy_params = strat_mgr_.all_param_values();
    cfg_.strategy_tourn_excluded = strat_mgr_.tournament_excluded_keys();
    // Capture the live broker's order-path latency so the optimizer models the
    // real venue instead of the 250 us default (consumed by sim_exec_latency).
    AckSummary ack{};
    if (ibkr_) ack = ibkr_->ack_latency();
    else if (tws_) ack = tws_->ack_latency();
    if (ack.count >= kMinLatSamples && ack.base_ns > 0) {
        cfg_.measured_lat_ns = ack.base_ns;
        cfg_.measured_lat_jitter_ns = ack.jitter_ns;
        cfg_.measured_lat_count = static_cast<int64_t>(ack.count);
    }
    cfg_.save(config_path_);
}

// Generate/persist the bearer token, then bind the read-only diagnostics socket.
// Called once from the constructor.
void App::start_diag_server() {
    if (!cfg_.diag_enabled) {
        route("diag: endpoint disabled (diag_enabled=false in config.json)");
        return;
    }
    if (cfg_.diag_token.empty()) {
        cfg_.diag_token = gen_diag_token();
        cfg_.save(config_path_);   // persist so the token is stable across restarts
    }
    const uint16_t port = static_cast<uint16_t>(cfg_.diag_port);
    // Remote control (opt-in): a SEPARATE token guards POST /control/kill. The
    // server thread only sets a flag — the actual kill_switch() runs on the UI
    // thread (see pump_diag), the sole producer to the engine's command ring.
    if (cfg_.diag_control_enabled) {
        if (cfg_.diag_control_token.empty()) {
            cfg_.diag_control_token = gen_diag_token();
            cfg_.save(config_path_);
        }
        diag_srv_.set_control(cfg_.diag_control_token, [this](const std::string& action) {
            if (action == "kill") {
                diag_kill_requested_.store(true, std::memory_order_relaxed);
                route("diag: REMOTE KILL-SWITCH requested via POST /control/kill");
                return std::string(
                    "{\"status\":\"kill queued\",\"note\":\"watch /diag halted+positions to confirm\"}");
            }
            return std::string("{\"error\":\"unknown control action\"}");
        });
    }
    diag_json_ = build_diag_json();   // seed so the first request isn't "{}"
    metrics_text_ = build_metrics();  // seed /metrics too
    diag_srv_.set_metrics(
        [this] { std::lock_guard<std::mutex> g(diag_mu_); return metrics_text_; });
    diag_srv_.set_tail([this](uint64_t& cursor) { return build_logs_sse(cursor); });
    const bool ok = diag_srv_.start(
        cfg_.diag_bind, port, cfg_.diag_token,
        [this] { std::lock_guard<std::mutex> g(diag_mu_); return diag_json_; },
        [this](uint64_t since) { return build_logs_json(since); },
        [] { return std::string(diag_html()); },
        [this](std::string l) { route(std::move(l)); });
    if (ok) {
        route("diag: read-only endpoint on " + cfg_.diag_bind + ":" +
                 std::to_string(port) + "  —  browse  http://<tailscale-ip>:" +
                 std::to_string(port) + "/?token=" + cfg_.diag_token);
        if (cfg_.diag_control_enabled)
            route("diag: REMOTE CONTROL ENABLED — POST /control/kill flattens + halts; "
                     "control token is in config.json (diag_control_token)");
    } else
        route("diag: endpoint failed to start (is port " + std::to_string(port) +
                 " already in use?)");
}

// UI thread, per TICK (no frame in progress — the 1 Hz gate is on mono_s(), not
// ImGui's clock): re-render the /diag body into diag_json_, which the server
// thread copies out under diag_mu_.
void App::pump_diag() {
    // Execute a remote kill request here, on the UI thread — the only producer
    // allowed to touch the engine's SPSC command ring (the diag server thread
    // just set the flag).
    if (diag_kill_requested_.exchange(false, std::memory_order_relaxed)) {
        if (engine_.live_running()) {
            engine_.kill_switch();
            route("diag: KILL-SWITCH EXECUTED — cancel all + flatten + halt");
        } else {
            route("diag: kill-switch requested but no live session is running");
        }
    }
    if (!diag_srv_.running()) return;
    const double now = mono_s();
    if (now < diag_next_build_s_) return;
    diag_next_build_s_ = now + 1.0;
    std::string body = build_diag_json();
    std::string metrics = build_metrics();
    std::lock_guard<std::mutex> g(diag_mu_);
    diag_json_ = std::move(body);
    metrics_text_ = std::move(metrics);
}

// Render the current diagnostics snapshot as JSON. UI thread only (reads
// engine_.live_snapshot() and cfg_ the same way the panels do).
std::string App::build_diag_json() {
    using nlohmann::json;
    const LiveSnapshot s = engine_.live_snapshot();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

    json j;
    // ---- build / session identity ----
    j["version"] = TT_VERSION;   // clean semver, e.g. "0.1.5"
    j["engine_version"] = engine_version();
    j["git_commit"] = TT_GIT_COMMIT;
    j["git_dirty"] = TT_GIT_DIRTY;
    j["build_date"] = TT_BUILD_DATE;
    j["session_start"] = iso_utc(session_start_);
    j["uptime_sec"] =
        session_start_ ? static_cast<int64_t>(std::time(nullptr) - session_start_) : 0;
    j["now"] = iso_utc(std::time(nullptr));
    j["control"] = cfg_.diag_control_enabled ? "enabled" : "read-only";
    j["route"] = cfg_.trade_route == 1 ? "TWS socket" : "IBKR web";
    // broker_connected reflects whether orders can actually reach IBKR: the
    // local API socket up (ready) AND, on the TWS route, the gateway's upstream
    // link to IBKR up (not in an error-1100 state — e.g. an IBKR maintenance /
    // weekend reset leaves the socket up but the upstream down). broker_upstream
    // is broken out so a maintenance blip is distinguishable from a socket drop.
    const bool broker_ready = ibkr_ ? ibkr_->ready() : (tws_ ? tws_->ready() : false);
    const bool broker_upstream = tws_ ? tws_->upstream_connected() : true;
    j["broker_connected"] = broker_ready && broker_upstream;
    j["broker_upstream"] = broker_upstream;
    // Which of our three TWS clients is being refused because something already
    // holds its API client id (IB error 326 — engine/tws_client_id.h). A
    // non-empty list means those clients are DOWN AND RETRYING (slowly), not
    // that they gave up: the entry disappears when the handshake completes.
    // Read together with broker_connected it is the difference between "the
    // gateway is unreachable" and "the gateway is fine and refusing us", which
    // is exactly the distinction Watch-IbGateway.ps1 needs before it cold-
    // restarts a healthy gateway. Named rather than a bare bool because "orders"
    // and "market data" are very different days, and reported as a list so a
    // reader sees at a glance whether the collision took one client or all three.
    //
    // It exists here because on 2026-08-11 this failure was invisible outside
    // the log file: broker_connected read false, data.connected read false, and
    // nothing anywhere said the cause was a second TradeTerminal rather than a
    // gateway that was down.
    {
        json conflict = json::array();
        if (data_.client_id_conflict()) conflict.push_back("data");
        if (tws_ && tws_->client_id_conflict()) conflict.push_back("orders");
        if (tws_feed_ && tws_feed_->client_id_conflict()) conflict.push_back("feed");
        j["client_id_conflict"] = std::move(conflict);
    }

    // ---- session state ----
    j["live_running"] = s.running;
    j["halted"] = s.halted;
    j["cash"] = s.cash;
    j["equity"] = s.equity;
    j["ticks"] = s.ticks;
    j["dropped_ticks"] = s.dropped_ticks;
    j["feed_stale_ms"] = s.last_tick_ts_ms > 0 ? (now_ms - s.last_tick_ts_ms) : -1;
    j["stuck_orders"] = tws_ ? tws_->stuck_order_count() : 0;   // half-open (TWS route)
    // ---- does the app's book still match the ACCOUNT's? ----
    //
    // A first-class field because on 2026-08-13 a phantom 411-share position was
    // published here as real, with a fictional +$528 unrealized and
    // unprotected=false, for 4 h 03 m — every other number on this page agreed
    // with it, because every other number is derived from the same book.
    //
    // It is a STRING, not a count, and that is the design. A count reads 0 both
    // when the books agree and when nothing has answered a position request
    // since 09:31 — the exact shape of failure that pinned oldest_history_age_ms
    // at 0 through a five-hour outage and left data.connected true against a
    // login modal. The values, exhaustively:
    //
    //   "off"        no live session, or a route with no audit path (ibkr_web)
    //   "starting"   armed; the first answer has not come back yet
    //   "ok"         the last answer came back and AGREED (the only healthy read)
    //   "suspect: RAM app=411 broker=0 (app-only) (1/2)"   seen once, unconfirmed
    //   "DIVERGED: RAM app=411 broker=0 (app-only)"        confirmed; this pages
    //   "unknown: no broker answer for 312s"               the auditor is blind
    //
    // "ok" is unreachable without a fresh, agreeing answer. Every failure —
    // including ones nobody has thought of — reads as something else, and
    // book_audit_agreed_age_ms below climbs in all of them.
    // DID THE PAGES ACTUALLY ARRIVE? An alert the operator never received is
    // indistinguishable, from inside the app, from one that was never worth
    // sending — until 0.29.2 the webhook POST discarded its result entirely.
    // On 2026-08-17 the two Critical pages of the day (an off-lineup broker
    // position, a confirmed book divergence) were emitted, logged, and never
    // delivered; nothing anywhere recorded that. `failed` non-zero means the
    // phone is not a reliable channel right now, whatever the log says.
    {
        const AlertNotifier::Delivery d = alerts_.delivery();
        nlohmann::json a;
        a["sent"] = d.sent;
        a["failed"] = d.failed;
        a["retried"] = d.retried;
        a["dropped"] = d.dropped;
        a["evicted"] = d.evicted;
        // 0.30.0. What the flood defence swallowed, and how it was reported.
        // `coalesced` is repeats folded into a page already sent; `throttled` is
        // the per-severity rate cap refusing distinct alerts; `summaries` is the
        // pages that told the operator about both. A rising coalesced/throttled
        // with summaries stuck at zero would mean suppression had become
        // silence, which is the failure this layer was built to avoid.
        a["coalesced"] = d.coalesced;
        a["throttled"] = d.throttled;
        a["summaries"] = d.summaries;
        // THE CHANNEL CAN BE TURNED OFF FROM A MENU. When it is, every page —
        // Criticals included — is discarded, and until 0.31.2 nothing recorded
        // it, so a muted app looked exactly like a quiet one.
        a["muted"] = alerts_.muted();
        a["muted_discarded"] = d.muted_discarded;
        // WHICH CATEGORIES ARE OFF, and what that has cost so far. Keyed by NAME
        // so the answer survives a reordering of the enum, and only categories
        // actually switched off appear — a fully-armed channel says nothing
        // here, so anything present is a deliberate silence.
        nlohmann::json off = nlohmann::json::object();
        for (int i = 0; i < static_cast<int>(AlertCategory::COUNT); ++i)
            if (!alerts_.category_enabled(i))
                off[alert_category_name(static_cast<AlertCategory>(i))] =
                    d.category_discarded[i];
        a["categories_off"] = std::move(off);
        a["last_status"] = d.last_status;
        a["last_error"] = d.last_error;
        j["alerts"] = std::move(a);
    }
    {
        const int64_t steady_now_ms = steady_ms();
        j["book_divergence"] = book_audit_.field(steady_now_ms);
        // ms since an audit last came back AND agreed, counting from the moment
        // the auditor armed if none ever has; -1 = no live session. The one
        // number that means the same thing in every state — and, since the
        // review of this change, one that actually CLIMBS in the state it was
        // chosen for (armed and never answered used to read -1 forever).
        j["book_audit_agreed_age_ms"] = book_audit_.last_agreed_age_ms(steady_now_ms);
        j["book_audit_rounds"] = book_audit_.rounds();
        // What the operator was paged about and has not been told is over. It
        // outlives `book_divergence` slipping to suspect/unknown, which are
        // exits from DIVERGED that are not recoveries — see net::DivergenceLatch.
        j["book_divergence_open"] =
            static_cast<int>(book_div_.divergences().size());
    }
    // Non-zero STOCK positions the LAST reconcile found in symbols this session
    // does not trade. A first-class field for the same reason book_divergence is
    // one: for 24 days this number was structurally unobservable — reconciliation
    // discarded those rows above its own log line, so the log could not even
    // distinguish "IB never sent it" from "we threw it away" — and 20 NVDA shares
    // sat in the account with no strategy, no stop, and nothing able to flatten
    // them. -1 = no TWS route (the IBKR web route has no position stream at all,
    // which is a different answer from "none found"). See
    // engine/reconcile_policy.h.
    j["reconcile_offlineup"] = tws_ ? tws_->reconcile_offlineup() : -1;
    // Connect-timeout watchdog force-aborts (broker + feed). Nonzero = a gateway
    // handshake wedged and self-healed instead of freezing the I/O thread.
    j["connect_aborts"] = (tws_ ? tws_->connect_aborts() : 0) +
                          (tws_feed_ ? tws_feed_->connect_aborts() : 0);

    // ---- market-data source (candles for chart/backtest/optimizer + quotes) ----
    // Distinct from broker_connected/feed_stale_ms above (live order + tick
    // path): this is the historical-candle session the tournament/optimizer
    // depends on. oldest_history_age_ms climbing while connected is a half-open
    // data session — requests issued, answers silently dropped.
    //
    // last_bar_age_ms below is the answer to the 2026-08-07 outage, where all
    // three fields above looked perfect for five hours: connected stayed true
    // on a half-open socket, and oldest_history_age_ms never grew because the
    // failing requests were CANCELLED at 20s and left the pending set. Anything
    // keyed off in-flight requests is blind to that by construction, so this
    // measures the last SUCCESS instead. See net/hist_freshness.h.
    const std::string bar_ivl = traded_bar_interval();
    const int64_t steady_now = steady_ms();
    {
        json d;
        d["source"] = use_tws_data_ ? "tws" : "ibkr_web";
        d["connected"] = data_.connected();
        // connected=false has two very different causes and they need different
        // hands: a gateway that is down (wait / restart it) versus something
        // else holding our client id (find it and close it). This one is still
        // retrying every kTwsClientIdRetrySec, so it can clear on its own — a
        // reader must not treat it as a permanent state.
        d["client_id_conflict"] = data_.client_id_conflict();
        d["pending_history"] = data_.pending_history();
        d["oldest_history_age_ms"] = data_.oldest_history_age_ms();
        // Is the GATEWAY logged in to IBKR, as opposed to merely listening?
        // The one overnight health signal there is: broker_connected above is
        // false all night by design (no live session, so the orders client is
        // deliberately disconnected), and on 2026-08-09 a gateway parked on a
        // failed-login dialog kept both its process and port 4002 up for 13
        // hours with connected=true here. Proven by the data farms, so it is
        // TWS-route only — on ibkr_web it reads false/-1/0 always.
        // See net/gateway_auth.h.
        d["gateway_authed"] = data_.gateway_authed();
        d["farms_ok_age_ms"] = data_.gateway_auth_age_ms();   // -1 = never proven
        d["farms_ok"] = data_.gateway_farms_ok();             // 0..3 currently up
        // Which series the ages refer to: the one the autopilot re-fetches.
        d["bar_interval"] = bar_ivl;
        std::vector<std::string> live_syms;
        live_syms.reserve(s.symbols.size());
        for (const auto& ss : s.symbols) live_syms.push_back(ss.symbol);
        // Worst age over every live symbol; -1 = none has ever been answered
        // (idle session, or a data client broken since startup). Broader than
        // what pump_history_watchdog pages on, which is limited to
        // autopilot-armed symbols — nothing re-fetches an unarmed symbol's bars
        // after the session-start warmup, so a large figure here can be by
        // design. The per-symbol ages below say which case it is.
        d["worst_last_bar_age_ms"] =
            hist_fresh_.worst_age_ms(live_syms, bar_ivl, steady_now);
        j["data"] = std::move(d);
    }

    auto strat_for = [&](const std::string& sym) -> const TradeSymbol* {
        for (const auto& ts : cfg_.trade_symbols)
            if (ts.symbol == sym) return &ts;
        return nullptr;
    };
    // A symbol is "unprotected" when it holds a nonzero position but has no
    // Working stop order — the async-reject gap that leaves a naked position.
    auto has_working_stop = [&](const std::string& sym) {
        for (const auto& o : s.orders)
            if (o.symbol == sym && o.status == OrderStatus::Working &&
                o.type == static_cast<uint8_t>(OrdType::Stop))
                return true;
        return false;
    };

    json syms = json::array();
    int unprotected = 0;
    for (const auto& ss : s.symbols) {
        json e;
        e["symbol"] = ss.symbol;
        e["last_price"] = ss.last_price;
        e["position"] = ss.position.qty;
        e["avg_price"] = ss.position.avg_price;
        e["unrealized_pnl"] = ss.position.unrealized_pnl;
        e["realized_pnl"] = ss.position.realized_pnl;
        // realized_pnl is GROSS of commissions — Portfolio::apply books
        // (price - avg) * qty and charges the fee to cash only. 2026-08-14
        // reported -$597.01 against -$612.09 of actual cash on 13 fills. Both
        // are published (the gross figure is what every historical journal row
        // and saved session was computed with, so redefining it silently would
        // break comparability); net is the one to judge edge on.
        e["fees"] = ss.fees;
        e["realized_pnl_net"] = ss.position.realized_pnl - ss.fees;
        const bool naked = ss.position.qty != 0.0 && !has_working_stop(ss.symbol);
        e["unprotected"] = naked;
        if (naked) ++unprotected;
        // TWO ages, because they answer different questions and one of them used
        // to masquerade as the other. last_delivery_age_ms is ms since this
        // symbol's last SUCCESSFUL bar delivery (SOXS read 09:17 all afternoon on
        // 2026-08-07 while every other field looked healthy). last_bar_age_ms is
        // the age of the newest BAR in that delivery — which is what this field
        // has always been NAMED and never held. On 2026-08-14 STKH's 5m series
        // was frozen at x10010 bars for five hours while its deliveries arrived
        // on schedule: perfect on the delivery age, five hours stale on the data.
        // -1 in either = never answered in this process.
        e["last_delivery_age_ms"] = hist_fresh_.age_ms(ss.symbol, bar_ivl, steady_now);
        e["last_bar_age_ms"] = hist_fresh_.bar_age_ms(ss.symbol, bar_ivl, now_ms);
        // Params come from the ENGINE, not from cfg_.trade_symbols. The Trade
        // tab's copy is overwritten by every crowned champion, while the live
        // engine only takes one when autopilot's improvement test passes — so
        // the terminal's copy is a PROPOSAL and reporting it as live state was
        // how /diag came to show parameters that were never trading. When the
        // two disagree, say so rather than picking one silently.
        e["params"] = ss.params;
        // Where those values came from, latched at session start: "own" (fitted
        // to THIS symbol), "mixed", "inherited" (the strategy's shared defaults
        // — nothing fitted to this symbol at all), "none". Six symbols showing
        // identical "params" with "inherited" is the 2026-08-10 failure, stated
        // instead of left for a human to notice.
        // Latched at start, so it only describes a RUNNING session — reporting
        // it afterwards would label config values with the last session's source.
        if (engine_.live_running())
            if (const auto psrc = live_param_source_.find(ss.symbol);
                psrc != live_param_source_.end())
                e["params_source"] = psrc->second;
        if (const TradeSymbol* ts = strat_for(ss.symbol)) {
            // NAME THE STRATEGY THAT ACTUALLY RAN. "" is not a separate built-in
            // any more — App::acquire_strategy maps it to kBuiltinStrategyKey
            // ("sma_crossover.cpp"), a real promoted strategy that sizes off
            // ctx.budget() like every other one. The old literal survived that
            // merge and made /diag report a strategy which no longer exists.
            //
            // Not cosmetic: on 2026-08-20 this label alone led an analysis of a
            // live session to conclude SOXL was running a fixed-qty demo that
            // bypassed risk sizing. The log said "SOXL: SMA(dll)" — the truth —
            // and /diag said "built-in SMA". Same family as the journal's
            // mode='sim' (0.31.3): a display string left behind by a refactor,
            // in a field someone reads to decide whether something is wrong.
            e["strategy"] = ts->strat_key.empty() ? kBuiltinStrategyKey : ts->strat_key;
            if (!engine_.live_running() || ss.params.empty())
                e["params"] = ts->params;   // not live yet: config is all there is
            else if (ts->params != ss.params)
                e["params_proposed"] = ts->params;   // queued/rejected, NOT trading
        }
        syms.push_back(std::move(e));
    }
    j["symbols"] = std::move(syms);
    j["unprotected_positions"] = unprotected;

    // ---- rejects feed (count + recent, with the reason) --------------------
    // Every row carries reject_cause, and reject_cause is never "none" on a
    // rejected order (engine/reject.h). Before 0.23.0 this block could and did
    // emit reject_code 0 with reject_msg "", which is what the operator found on
    // 2026-08-13 and could do nothing with.
    int reject_count = 0;
    for (const auto& o : s.orders)
        if (o.status == OrderStatus::Rejected) ++reject_count;
    json rejects = json::array();
    for (auto it = s.orders.rbegin(); it != s.orders.rend() && rejects.size() < 10; ++it)
        if (it->status == OrderStatus::Rejected) {
            json r;
            r["id"] = it->id;
            r["symbol"] = it->symbol;
            r["side"] = it->side == static_cast<uint8_t>(Side::Buy) ? "buy" : "sell";
            r["type"] = order_type_name(it->type);
            r["qty"] = it->qty;
            r["limit_price"] = it->limit_price;
            r["reject_cause"] = reject_cause_slug(it->reject_cause);   // greppable
            r["reject_broker"] = reject_cause_is_broker(it->reject_cause);
            r["reject_code"] = it->reject_code;   // the BROKER's number; 0 = local
            r["reject_msg"] = it->reject_msg;     // never empty on a reject
            rejects.push_back(std::move(r));
        }
    j["reject_count"] = reject_count;
    j["rejects_recent"] = std::move(rejects);

    // ---- refusal table (every refusal, counted, per symbol and cause) ------
    // The complete picture the blotter cannot give: it only records the FIRST
    // refusal of each (symbol, cause) pair so a retrying strategy cannot evict
    // the session's real orders from a 200-entry ring.
    //
    // HEALTHY: exactly one row per symbol, cause "warmup_replay", last_ts_ms
    // pinned to session start. BROKEN: any other cause present, or a
    // warmup_replay row whose last_ts_ms keeps advancing (set_warming never
    // cleared - a whole session silently placing nothing).
    json refusals = json::array();
    uint64_t refusals_total = 0, refusals_exit = 0;
    for (const auto& rf : s.refusals) {
        refusals_total += rf.count;
        // exit_count, NOT count: a row is keyed on (symbol, cause) and its
        // exit_order flag is sticky, so summing count reported every ENTRY
        // refusal that shared a row with one exit. And the two causes that
        // refuse exits BY DESIGN are excluded outright (engine/reject.h):
        // counting them made a field documented as "there is no benign value
        // but 0" read 189 for one held position.
        if (!reject_cause_exit_is_by_design(rf.cause)) refusals_exit += rf.exit_count;
        json r;
        r["symbol"] = rf.symbol;
        r["cause"] = reject_cause_slug(rf.cause);
        r["count"] = rf.count;
        r["exit_order"] = rf.exit_order;   // true = an exit was refused
        r["exit_count"] = rf.exit_count;   // how many of `count` were exits
        r["code"] = rf.code;
        r["message"] = rf.message;
        r["first_ts_ms"] = rf.first_ts_ns / 1'000'000;
        r["last_ts_ms"] = rf.last_ts_ns / 1'000'000;
        refusals.push_back(std::move(r));
    }
    j["refusals"] = std::move(refusals);
    j["refusal_count"] = refusals_total;
    // Nonzero = at least one order that would have REDUCED or CLOSED a position
    // was refused. There is no benign value but 0.
    j["refused_exits"] = refusals_exit;

    // ---- entry gate --------------------------------------------------------
    // armed false on a live session = the app did not install a gate and
    // strategies may enter at any hour (the 2026-08-13 configuration).
    // open reads true only inside 09:30-15:57 on a real trading day; true at
    // 18:00 or on a Sunday means the calendar behind it is wrong.
    // blocked counts entries the gate refused: 0 on a healthy day, and the
    // number that did not exist at all before 0.23.0.
    {
        json g;
        g["armed"] = s.entry_gate_armed;
        g["open"] = s.entry_gate_open;
        g["blocked"] = s.entry_gate_blocked;
        j["entry_gate"] = std::move(g);
    }

    // ---- risk-halt headroom (how close the session is to an automated halt) ----
    // nearest_halt_frac is 0 (safe) .. 1 (at the halt), the max proximity across
    // the armed equity limits — one at-a-glance "distance to halt" number.
    {
        const auto& rk = s.risk;
        json risk;
        risk["daily_loss"] = rk.daily_loss;
        risk["daily_loss_limit"] = rk.daily_loss_limit;
        risk["drawdown_pct"] = rk.drawdown_pct;
        risk["drawdown_limit_pct"] = rk.drawdown_limit_pct;
        risk["stale_feed_sec"] = rk.stale_feed_sec;
        double nearest = 0.0;
        const char* which = "none";
        if (rk.daily_loss_limit > 0) {
            risk["daily_loss_room"] = rk.daily_loss_limit - rk.daily_loss;
            const double f = rk.daily_loss / rk.daily_loss_limit;
            if (f > nearest) { nearest = f; which = "daily_loss"; }
        }
        if (rk.drawdown_limit_pct > 0) {
            risk["drawdown_room_pct"] = rk.drawdown_limit_pct - rk.drawdown_pct;
            const double f = rk.drawdown_pct / rk.drawdown_limit_pct;
            if (f > nearest) { nearest = f; which = "drawdown"; }
        }
        risk["nearest_halt"] = which;
        risk["nearest_halt_frac"] = nearest < 0.0 ? 0.0 : nearest;   // clamp (up on day)
        j["risk"] = std::move(risk);
    }

    // ---- latency ----
    AckSummary ack{};
    if (ibkr_) ack = ibkr_->ack_latency();
    else if (tws_) ack = tws_->ack_latency();
    json lat;
    lat["ack_count"] = ack.count;
    lat["ack_p50_ms"] = ack.base_ns / 1'000'000.0;
    lat["ack_p90_ms"] = (ack.base_ns + ack.jitter_ns) / 1'000'000.0;
    lat["tick_to_order_p50_us"] = s.lat_p50 / 1000.0;
    lat["tick_to_order_p99_us"] = s.lat_p99 / 1000.0;
    lat["tick_to_order_max_us"] = s.lat_max / 1000.0;
    lat["tick_to_order_count"] = s.lat_count;
    j["latency"] = std::move(lat);

    return j.dump(2);
}

// Prometheus exposition text (GET /metrics). Built on the UI thread in
// pump_diag alongside the /diag JSON — same threading contract (reads cfg_ and
// the live snapshot the way the panels do), published under diag_mu_.
std::string App::build_metrics() {
    const LiveSnapshot s = engine_.live_snapshot();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    AckSummary ack{};
    if (ibkr_) ack = ibkr_->ack_latency();
    else if (tws_) ack = tws_->ack_latency();

    int reject_count = 0;
    for (const auto& o : s.orders)
        if (o.status == OrderStatus::Rejected) ++reject_count;

    // Escape a Prometheus label value (backslash, double-quote, newline).
    auto lbl = [](const std::string& v) {
        std::string o;
        o.reserve(v.size());
        for (char ch : v) {
            if (ch == '\\' || ch == '"') { o += '\\'; o += ch; }
            else if (ch == '\n') o += "\\n";
            else o += ch;
        }
        return o;
    };

    std::string m;
    m.reserve(2048);
    auto g = [&](const char* name, const char* help, double val) {
        m += "# HELP "; m += name; m += ' '; m += help; m += '\n';
        m += "# TYPE "; m += name; m += " gauge\n";
        m += name; m += ' '; m += fmt_num(val); m += '\n';
    };
    g("tt_up", "1 while the terminal process is serving metrics", 1);
    g("tt_live_running", "1 if a live session is active", s.running ? 1 : 0);
    g("tt_halted", "1 if the session is halted", s.halted ? 1 : 0);
    g("tt_broker_connected", "1 if the broker can reach IBKR (socket + upstream)",
      ((ibkr_ ? ibkr_->ready() : (tws_ ? tws_->ready() : false)) &&
       (tws_ ? tws_->upstream_connected() : true))
          ? 1 : 0);
    // The overnight signal. tt_broker_connected is 0 all night BY DESIGN, so
    // this is the only gauge that can distinguish a gateway that is logged in
    // from one parked on a failed-login dialog (2026-08-09; gateway_auth.h).
    // TWS route only — 0 / -1 on the ibkr_web route, where nothing proves it.
    g("tt_gateway_authed", "1 if IB Gateway is proven logged in to IBKR (TWS route)",
      data_.gateway_authed() ? 1 : 0);
    g("tt_gateway_auth_age_ms",
      "ms since the last proof the gateway is logged in (-1 = never)",
      static_cast<double>(data_.gateway_auth_age_ms()));
    g("tt_equity_dollars", "account equity", s.equity);
    g("tt_cash_dollars", "account cash", s.cash);
    g("tt_ticks_total", "market-data ticks this session", static_cast<double>(s.ticks));
    g("tt_dropped_ticks_total", "ticks dropped (ring full)",
      static_cast<double>(s.dropped_ticks));
    g("tt_reject_count", "orders rejected this session", reject_count);
    // Refusals include the ones that never became a blotter row (a strategy
    // retrying into the same gate), so this can exceed tt_reject_count. The
    // exit gauge is the one worth an alerting rule: any nonzero value means an
    // order that would have closed a position did not get placed.
    {
        double total = 0, exits = 0;
        for (const auto& rf : s.refusals) {
            total += static_cast<double>(rf.count);
            // See the matching sum in /diag: exit_count, not count, and never a
            // by-design exit refusal — an alerting rule on this gauge fired
            // permanently in hold-until-profitable mode.
            if (!reject_cause_exit_is_by_design(rf.cause))
                exits += static_cast<double>(rf.exit_count);
        }
        g("tt_refusal_count", "orders refused this session, all causes", total);
        g("tt_refused_exits",
          "refused orders that would have reduced/closed a position", exits);
    }
    g("tt_entry_gate_armed", "1 if a session-hours entry gate is installed",
      s.entry_gate_armed ? 1 : 0);
    g("tt_entry_gate_open", "1 while strategies may open/add to positions",
      s.entry_gate_open ? 1 : 0);
    g("tt_entry_gate_blocked", "entries refused because the session was closed",
      static_cast<double>(s.entry_gate_blocked));
    // Any nonzero value is worth an alerting rule: the account holds stock this
    // session does not trade, cannot audit and cannot flatten, and until 0.25.0
    // reconciliation discarded those rows without so much as a log line — 20
    // NVDA shares for 24 days. -1 = no TWS route (nothing to measure), which is
    // a different answer from 0 (measured, none found).
    g("tt_reconcile_offlineup",
      "broker positions found outside this session's symbols (-1 = no TWS route)",
      tws_ ? static_cast<double>(tws_->reconcile_offlineup()) : -1.0);
    g("tt_feed_stale_ms", "ms since the last tick (-1 = none yet)",
      s.last_tick_ts_ms > 0 ? static_cast<double>(now_ms - s.last_tick_ts_ms) : -1);
    g("tt_stuck_orders", "orders unacked past the half-open threshold (TWS)",
      tws_ ? tws_->stuck_order_count() : 0);
    g("tt_connect_aborts", "connect-timeout watchdog force-aborts (broker+feed)",
      (tws_ ? tws_->connect_aborts() : 0) +
          (tws_feed_ ? tws_feed_->connect_aborts() : 0));
    // How many of the three TWS clients are currently being refused because
    // something already holds their API client id (IB error 326). They are still
    // retrying, so this is a gauge and not a death count — but a nonzero that
    // does NOT return to 0 within a minute or two needs a human, which is why it
    // belongs on a dashboard next to the gauges above rather than only in a log
    // file, where the whole of the 2026-08-11 incident lived.
    g("tt_client_id_conflicts", "TWS clients refused an in-use API client id",
      (data_.client_id_conflict() ? 1 : 0) +
          (tws_ && tws_->client_id_conflict() ? 1 : 0) +
          (tws_feed_ && tws_feed_->client_id_conflict() ? 1 : 0));
    g("tt_ack_latency_p50_ms", "order submit->ack p50", ack.base_ns / 1'000'000.0);
    g("tt_ack_latency_p90_ms", "order submit->ack p90",
      (ack.base_ns + ack.jitter_ns) / 1'000'000.0);
    g("tt_tick_to_order_p99_us", "tick->order p99", s.lat_p99 / 1000.0);
    g("tt_halt_proximity", "0 safe .. 1 at the nearest armed equity halt", [&] {
        double n = 0.0;
        if (s.risk.daily_loss_limit > 0)
            n = std::max(n, s.risk.daily_loss / s.risk.daily_loss_limit);
        if (s.risk.drawdown_limit_pct > 0)
            n = std::max(n, s.risk.drawdown_pct / s.risk.drawdown_limit_pct);
        return n < 0.0 ? 0.0 : n;
    }());

    // Book divergence, for a remote alert rule. Two gauges rather than one
    // because the two questions are different and a single number cannot answer
    // both: "do the books disagree?" and "is anyone still checking?". An alert
    // on the count alone would go quiet the moment the auditor died, which is
    // exactly how this codebase has been blinded before — so the AGE is the one
    // to page on, and it climbs in every failure mode. It really does now: an
    // armed auditor that has never been answered ages from ARMING rather than
    // reporting -1, which used to read below every threshold a rule could write
    // (see BookAudit::last_agreed_age_ms). -1 now means only "no live session".
    //
    // The count is the LATCHED set, matching the page: BookAudit::confirmed()
    // empties as soon as the state leaves Diverged, so a divergence still being
    // paged about every 15 minutes would have exported 0 here.
    g("tt_book_divergences",
      "live positions the broker and the app disagree about (confirmed, and not "
      "since contradicted by an audit that agreed)",
      static_cast<double>(book_div_.divergences().size()));
    g("tt_book_audit_agreed_age_ms",
      "ms since a broker position audit last came back AND agreed, or since the "
      "auditor armed if none ever has (-1 = no live session)",
      static_cast<double>(book_audit_.last_agreed_age_ms(steady_ms())));

    // Bar staleness: ms since each symbol's last SUCCESSFUL history delivery.
    // Alert on this, not on tt_data_pending_history — a stalled request is
    // cancelled at 20s and never ages (see net/hist_freshness.h).
    {
        const std::string bar_ivl = traded_bar_interval();
        const int64_t steady_now = steady_ms();
        std::vector<std::string> live_syms;
        live_syms.reserve(s.symbols.size());
        for (const auto& ss : s.symbols) live_syms.push_back(ss.symbol);
        g("tt_worst_bar_stale_ms",
          "ms since the oldest traded symbol's last history delivery (-1 = none yet)",
          static_cast<double>(hist_fresh_.worst_age_ms(live_syms, bar_ivl, steady_now)));
        m += "# HELP tt_bar_stale_ms ms since this symbol's last history delivery "
             "(-1 = none yet)\n# TYPE tt_bar_stale_ms gauge\n";
        for (const auto& ss : s.symbols)
            m += "tt_bar_stale_ms{symbol=\"" + lbl(ss.symbol) + "\"} " +
                 fmt_num(static_cast<double>(
                     hist_fresh_.age_ms(ss.symbol, bar_ivl, steady_now))) +
                 '\n';
    }

    // Per-symbol position + unrealized pnl (labelled series).
    m += "# HELP tt_position Signed position size per symbol\n# TYPE tt_position gauge\n";
    for (const auto& ss : s.symbols)
        m += "tt_position{symbol=\"" + lbl(ss.symbol) + "\"} " +
             fmt_num(ss.position.qty) + '\n';
    m += "# HELP tt_unrealized_pnl_dollars Unrealized P&L per symbol\n"
         "# TYPE tt_unrealized_pnl_dollars gauge\n";
    for (const auto& ss : s.symbols)
        m += "tt_unrealized_pnl_dollars{symbol=\"" + lbl(ss.symbol) + "\"} " +
             fmt_num(ss.position.unrealized_pnl) + '\n';
    return m;
}

// Incremental log tail for GET /logs. Runs on the server thread — safe because
// LogConsole::slice_since is mutex-guarded. The client passes back `next` as the
// next ?since=, and `dropped` flags a gap (it fell behind the 5000-line ring).
std::string App::build_logs_json(uint64_t since) {
    const LogConsole::Slice sl = log_.slice_since(since);
    nlohmann::json j;
    j["next"] = sl.next_id;
    j["dropped"] = sl.dropped;
    j["lines"] = sl.lines;
    return j.dump();
}

// SSE framing of the log tail (GET /events). Runs on a per-stream server thread;
// LogConsole::slice_since is mutex-guarded. Each new line is its own data:
// field and the batch is stamped id:<next>, so an EventSource reconnect resumes
// via Last-Event-ID with no gap or duplication. "" = nothing new (the server
// sends a keepalive comment instead).
std::string App::build_logs_sse(uint64_t& cursor) {
    const LogConsole::Slice sl = log_.slice_since(cursor);
    cursor = sl.next_id;
    if (sl.lines.empty() && !sl.dropped) return {};
    std::string out;
    if (sl.dropped) out += "event: dropped\ndata: older lines dropped\n\n";
    if (!sl.lines.empty()) {
        for (const std::string& ln : sl.lines) {
            out += "data: ";
            for (char ch : ln) out += (ch == '\r' || ch == '\n') ? ' ' : ch;  // one line
            out += '\n';
        }
        out += "id: " + std::to_string(sl.next_id) + "\n\n";
    }
    return out;
}

// One scheduling tick. NO ImGui, NO rendering, NO frame in progress.
//
// This is everything App::draw() used to do before it drew anything, and it is
// here rather than there because main.cpp used to skip draw() entirely whenever
// the window was ICONIFIED — so on a VPS operated over RDP, where a minimized
// window is an ordinary state, minimizing froze the 09:35 lineup build, the
// 09:25 auto-start, the 15:55 auto-stop, all four watchdogs and the autopilot at
// once. Silently: the gateway's own thread kept logging so the log looked alive,
// and /diag kept serving its last-built body with HTTP 200 (only the "now" field
// gave it away, and nothing compares that to real time). Observed 2026-08-11,
// three minutes into a dry run's build.
//
// TWO RULES hold this function together, and both are easy to break by accident:
//
//   - Nothing called from here may touch ImGui. There is no frame. Panel
//     ACCESSORS are fine (they read plain members); panel draw() methods are not.
//   - Every deadline evaluated here is on mono_s(), never ImGui::GetTime().
//     ImGui's clock does not advance without a NewFrame and then jumps by the
//     whole gap on the next one, so leaving a pump on it would swap this freeze
//     for every deadline in the app expiring at the same instant on restore —
//     including pump_lineup_swap's flatten give-up, whose branch kill-switches.
//     See tick_clock.h.
//
// Returns how long the host may sleep before the next tick (tick_sleep_ms).
int App::tick() {
    const int64_t t0 = mono_ms();
    const double now = mono_s();
    if (now - last_cfg_save_ > 60.0) {
        last_cfg_save_ = now;
        save_config();
    }
    pump_diag();   // re-render the /diag body (throttled) for the remote monitor

    // Surface engine/strategy/broker/feed log lines in the console; scan
    // them for alert-worthy events on the way through.
    std::string line;
    bool from_live = false;
    while (engine_.pop_log(line, from_live)) {
        // Route on the line's ORIGIN, not on global state. Backtest strategy
        // logs flood in during a sweep/tournament/lineup and belong in the
        // optimizer panel; everything the live session emits belongs in the
        // console. The old test — "is anything optimizing?" — was true for most
        // of the trading day because of the 30-minute autopilot, so live fills,
        // strategy entries and risk lines were being filed into the 58 MB
        // optimizer.log where nobody would find them.
        //
        // A SIMULATED EVENT MUST NEVER PAGE. alert_scan used to sit ABOVE this
        // branch, so it ran on every line the ring produced — and the engine
        // that runs the optimizer's backtests is the SAME engine object the live
        // session uses, so a strategy refused inside a replay woke the operator's
        // phone exactly like a refusal on the real book. The origin flag needed
        // to answer that question was already here, one line below, being used
        // for nothing but which panel the text landed in.
        //
        // It went off on 2026-08-17: 73 pages in 90 seconds, all
        // "[strategy warn] entry rejected", every one of them from a
        // donchian_trend that exists only inside a sweep cell. The trigger was
        // 0.28.0/0.29.0 giving replays the live entry gate and the live risk
        // limits — before that a backtest submit_order essentially never refused,
        // so the hole was there all along with nothing to fall into it. A
        // tournament runs thousands of replays; this scales with the fit, not
        // with anything happening to the account.
        if (from_live) {
            alert_scan(line);
            route(std::move(line));
        } else {
            opt_log_.add(std::move(line));
        }
    }
    if (ibkr_)
        while (ibkr_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    if (tws_)
        while (tws_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    if (tws_feed_)
        while (tws_feed_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    if (polygon_feed_)
        while (polygon_feed_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    if (finnhub_feed_)
        while (finnhub_feed_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    if (ibkr_feed_)
        while (ibkr_feed_->pop_log(line)) {
            alert_scan(line);
            route(std::move(line));
        }
    // Session over: stop streaming (frees the vendor connection slot).
    if ((polygon_feed_ || finnhub_feed_ || ibkr_feed_ || tws_feed_) &&
        !engine_.live_running()) {
        rt_feed_active_.store(false, std::memory_order_relaxed);
        polygon_feed_.reset();
        finnhub_feed_.reset();
        ibkr_feed_.reset();
        tws_feed_.reset();
        route("live: real-time feed stopped");
    }

    // TT_AUTORUN_BACKTEST=1: fire one AAPL backtest as soon as the feed is up
    // (headless end-to-end verification of the UI -> data -> engine path).
    if (!autorun_bt_done_ && data_.connected() && std::getenv("TT_AUTORUN_BACKTEST")) {
        autorun_bt_done_ = true;
        queue_backtest("", "AAPL", "1d", "2y", 100'000.0);   // built-in SMA
        route("autorun: queued AAPL 1d 2y backtest (built-in SMA)");
    }

    // TT_AUTORUN_SWEEP=1: auto-optimize the built-in SMA — headless
    // verification of the optimizer ("optimizer: finished ..." on success).
    if (!autorun_sweep_done_ && data_.connected() && std::getenv("TT_AUTORUN_SWEEP")) {
        autorun_sweep_done_ = true;
        SweepPanel::Request rq;
        rq.strat_key = "";   // built-in SMA
        rq.symbol = "AAPL";
        rq.interval = "1d";
        rq.range = "1y";
        rq.holdout_pct = 25;   // exercises the walk-forward phase headlessly
        queue_sweep(rq);
        route("autorun: queued auto-optimize (built-in SMA)");
    }

    // Journal: session rows are keyed off the live_running transition (works
    // for every start path — Trade panel, autorun, future ones), and fills
    // are drained afterwards so the session row always exists first.
    const bool live_now = engine_.live_running();
    if (!prev_live_running_ && live_now) {
        journal_syms_ = engine_.live_symbols();
        std::string jsyms;
        for (const std::string& sym : journal_syms_)
            jsyms += (jsyms.empty() ? "" : ",") + sym;
        // Baseline = starting equity. On a reconciling (TWS) route this is only
        // a placeholder — the broker hasn't replayed the real account yet — so
        // arm a re-anchor below once reconciliation completes. Otherwise (sim /
        // ibkr) the starting equity IS the baseline; leave it alone.
        const LiveSnapshot s0 = engine_.live_snapshot();
        journal_session_ = journal_.begin_session(
            jsyms, live_broker_mode(tws_ != nullptr, ibkr_ != nullptr), s0.equity);
        journal_baseline_pending_ =
            (tws_ && tws_->reconciles()) || (ibkr_ && ibkr_->reconciles());
    }
    // Drain fills into the (still-open) session BEFORE the stop transition can
    // close it below: end_session zeroes journal_session_ and add_fill(0,...) is
    // a no-op, so draining AFTER the stop would silently drop a session's closing
    // (flatten / kill-switch) fills — the very trades that realize its PnL.
    Engine::FillRecord fr;
    while (engine_.pop_fill(fr)) {
        const std::string sym = fr.symbol_id >= 1 && fr.symbol_id <= journal_syms_.size()
                                    ? journal_syms_[fr.symbol_id - 1]
                                    : "?";
        journal_.add_fill(journal_session_, fr.ts_ns, sym, fr.side == 1, fr.qty,
                          fr.price, fr.fee, fr.order_id);
    }
    if (prev_live_running_ && !live_now && journal_session_) {
        const LiveSnapshot s = engine_.live_snapshot();
        journal_.end_session(journal_session_, s.equity, s.halted);
        journal_session_ = 0;
        journal_baseline_pending_ = false;
    }
    // Re-anchor the session PnL baseline to the true account equity the instant
    // broker reconciliation finishes (adopted positions + real cash now loaded),
    // so per-session/day PnL is the session's actual trading delta, not a
    // ~$1M account swing measured off a ~$100k placeholder.
    if (journal_session_ && journal_baseline_pending_) {
        const LiveSnapshot s = engine_.live_snapshot();
        if (s.reconciled) {
            journal_.set_baseline(journal_session_, s.equity);
            journal_baseline_pending_ = false;
        }
    }
    prev_live_running_ = live_now;

    pump_sweep();   // before the panels: sweep results must not be stolen
    pump_tournament();
    pump_autopilot();
    pump_lineup_schedule();   // fire the daily build on the clock (before its pump)
    pump_lineup_swap();       // drive an in-progress live swap onto new picks
    pump_daily_lineup();
    // AFTER pump_daily_lineup, deliberately: the dry run reports the phase that
    // just ended, so it has to see this frame's transition rather than last
    // frame's. No-op unless TT_AUTORUN_LINEUP is set.
    pump_lineup_dryrun();
    pump_broker_watchdog();   // alert (webhook) if the order path drops mid-session
    pump_orphan_watchdog();   // alert if an adopted position has nothing closing it
    pump_book_audit();        // alert if the app's book and the broker's disagree
    pump_history_watchdog();  // alert if traded symbols' candles stop refreshing
    pump_preopen_gateway_check();   // 08:45 window: is the gateway actually LOGGED IN?
    pump_session_guard();     // a live session must not outlive its trading day

    // Deferred strategy loads, strategy-switch backtests, and finished-run
    // instance cleanup advance every tick, independent of open panels.
    strat_mgr_.pump();
    pump_pending_run();
    pump_leases();

    // The session schedule: auto-start at 09:25, auto-stop (kill switch) at 15:55.
    //
    // Lifted out of TradePanel::draw(), where it sat AFTER
    // `if (!ImGui::Begin("Trade", open)) return;`. Begin reports false for a
    // collapsed window AND for a docked tab that is not the selected one — and
    // setup_default_layout docks Trade into the same node as Backtest and
    // Strategy. So clicking either neighbouring tab silently disabled both the
    // auto-start and the auto-stop on a maximized, fully visible window, with
    // nothing anywhere to say the schedule had stopped running. Same defect
    // class as the iconify freeze, but it needed no minimize at all.
    //
    // The lambda BUILDS the options as well as starting: everything
    // build_start_opts needs is expensive to produce and was being produced on
    // every tick as call arguments, ahead of pump_schedule's own `if (!sched_on_)
    // return;`. trade_account_info() re-reads and JSON-parses ibkr-accounts.json
    // from disk; polygon_key()/finnhub_key() each run CryptUnprotectData twice
    // over the stored credential. At the 100 Hz tick that is millions of file
    // opens and decryptions a session, all of it discarded — and on the rendered
    // path it duplicated the read draw() already does below. Now it costs
    // nothing until the day's one 09:25 crossing actually fires.
    trade_.pump_schedule(
        [this] {
            start_live_session(trade_.build_start_opts(
                trade_account_info(), param_specs_fn(), !polygon_key().empty(),
                !finnhub_key().empty(), data_.connected()));
        },
        // The scheduled stop goes through the App, not the engine, so the TWS
        // adapter is actually torn down at 15:55 instead of holding the fixed
        // orders client id until the next morning's start collided with it.
        [this] { safe_stop_live(); });

    // Daily-lineup auto-start: a scheduled (non-propose-only) build reached Done,
    // so start the live session through the exact path the Start button uses.
    if (lineup_autostart_pending_) {
        lineup_autostart_pending_ = false;
        if (!trade_.has_symbols()) {
            route("lineup: no symbols to auto-start");
        } else {
            TradePanel::StartOpts opts = trade_.build_start_opts(
                trade_account_info(), param_specs_fn(),
                !polygon_key().empty(), !finnhub_key().empty(), data_.connected());
            if (engine_.live_running()) {
                // A session is already live: cycle onto the new picks, keeping
                // the symbols that carry over (only the drops get flattened).
                begin_lineup_swap(opts);
            } else {
                route("lineup: auto-starting the live session");
                // Say what will end it, at the moment it begins. On 2026-08-14
                // this branch started an unattended five-symbol session with
                // trade_sched_on false, i.e. with NOTHING on any clock able to
                // stop it, and said nothing about that — the session ran 1 h 47 m
                // past the close. pump_session_guard is the backstop now, so this
                // is no longer a refusal; it is the operator being told which of
                // the two is holding the rope.
                if (!trade_.sched_on())
                    route_operator(
                        "lineup: starting UNATTENDED with no scheduled stop armed "
                        "(trade_sched_on is off) - the session guard will stop it "
                        "shortly after the close. Arm the Trade panel schedule if you "
                        "want it stopped earlier.");
                start_live_session(opts);
            }
        }
    }

#ifdef TT_DEBUG
    // Debug menu (or TT_SIM_TICKS=1): synthesize a 2 Hz random walk for the
    // live session — demo/verification when the market is closed.
    if (engine_.live_running() && sim_ticks_) {
        if (now >= sim_tick_next_s_) {
            sim_tick_next_s_ = now + 0.5;
            if (sim_tick_px_ <= 0.0) sim_tick_px_ = 100.0;
            sim_tick_rng_ = sim_tick_rng_ * 1664525u + 1013904223u;
            sim_tick_px_ += (static_cast<double>(sim_tick_rng_ >> 8 & 0xffff) / 65535.0 - 0.5);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            for (const std::string& sym : engine_.live_symbols())
                engine_.push_live_tick(sym, ms, sim_tick_px_, 0.0);
        }
    }
#endif

    // TT_AUTORUN_LIVE=1: start a session, manual buy, kill switch — headless
    // verification of the whole live path.
    //
    // Not while TT_AUTORUN_LINEUP is set. This one calls engine_.start_live()
    // directly, so it is the one live-start path that does NOT go through
    // start_live_session()'s dry-run refusal — and "propose-only" has to mean it
    // whatever else the environment asks for.
    if (!dry_.active && std::getenv("TT_AUTORUN_LIVE")) {
        const LiveSnapshot s = engine_.live_snapshot();
        if (autorun_live_stage_ == 0) {
            autorun_live_stage_ = 1;
            LiveConfig cfg;
            cfg.symbols = {"SIMTEST", "SIMTEST2"};   // proves multi-symbol routing headlessly
            cfg.params = strat_mgr_.param_values("");   // built-in SMA
            cfg.bar_seconds = 2;
            if (IStrategy* strat = acquire_strategy("")) {
                // Both SIMTEST symbols run the same one strategy instance here.
                if (engine_.start_live(std::move(cfg),
                                       std::vector<IStrategy*>{strat, strat}))
                    leases_.push_back({strat, "", StrategyLease::Live});
                else
                    release_strategy({strat, "", StrategyLease::Live});
            }
            route("autorun-live: session started");
        } else if (autorun_live_stage_ == 1 && s.ticks >= 3) {
            autorun_live_stage_ = 2;
            engine_.submit_manual(1, true, 10);
            route("autorun-live: manual BUY 10 SIMTEST submitted");
        } else if (autorun_live_stage_ == 2 && !s.symbols.empty() &&
                  s.symbols[0].position.qty > 0) {
            autorun_live_stage_ = 3;
            route("autorun-live: position open, firing kill switch");
            engine_.kill_switch();
        } else if (autorun_live_stage_ == 3 && s.halted) {
            bool all_flat = true;
            for (const SymbolState& sym : s.symbols)
                if (sym.position.qty != 0) all_flat = false;
            if (all_flat) {
                autorun_live_stage_ = 4;
                route("autorun-live: FLAT after kill switch — live path verified");
            }
        }
    }

    return tick_sleep_ms(mono_ms() - t0);
}

// One frame of UI. Everything with a clock on it already ran in tick(); this
// only presents. main.cpp is free to skip it (iconified window) and the app must
// keep behaving identically — that invariant is the fix, so guard it: anything
// added here that DECIDES something rather than displaying it belongs in tick().
void App::draw() {
    const ImGuiID dockspace_id =
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    draw_menu_bar();
    draw_account_modal();
    draw_data_modal();
    draw_trading_guards();
    draw_update_panel();
    if (show_chart_) {
        // Overlay fills for the charted symbol: last backtest + live session.
        std::vector<FillMarker> fills;
        const std::string chart_sym = chart_.symbol();
        if (const BacktestResult* r = backtest_.result(); r && r->symbol == chart_sym)
            for (const TradeRow& t : r->fills)
                fills.push_back({static_cast<double>(t.ts_ns) / 1e9, t.price,
                                 t.side == static_cast<uint8_t>(Side::Buy)});
        double chart_live_px = 0.0, chart_live_ts = 0.0;
        if (engine_.live_running()) {
            const LiveSnapshot s = engine_.live_snapshot();
            for (const OrderRecord& o : s.orders)
                if (o.status == OrderStatus::Filled && o.symbol == chart_sym)
                    fills.push_back({static_cast<double>(o.ts_ns) / 1e9, o.fill_price,
                                     o.side == static_cast<uint8_t>(Side::Buy)});
            // Live tail for the charted symbol: its latest price + last tick time.
            for (const SymbolState& ss : s.symbols)
                if (ss.symbol == chart_sym && ss.last_price > 0.0) {
                    chart_live_px = ss.last_price;
                    chart_live_ts = static_cast<double>(s.last_tick_ts_ms) / 1000.0;
                    break;
                }
        }
        chart_.draw(&show_chart_, fills, chart_live_px, chart_live_ts);
    }
    if (show_watchlist_)
        watchlist_.draw(&show_watchlist_, [this](const std::string& sym) {
            chart_.show_symbol(sym);
            backtest_.set_symbol(sym);
        });
    if (show_backtest_)
        backtest_.draw(&show_backtest_, strat_mgr_.all_keys(),
                       [this](const std::string& k) { return strat_mgr_.display_name(k); },
                       [this](const std::string& k) { return strat_mgr_.loaded_fresh(k); },
                       pending_run_.active || strat_mgr_.load_pending(),
                       sweep_.running,
                       [this](const std::string& src, const std::string& sym,
                              const std::string& ivl, const std::string& rng,
                              double cash) {
                           queue_backtest_as(src, sym, ivl, rng, cash);
                       });
    if (show_replay_)
        replay_.draw(&show_replay_, strat_mgr_.loaded_keys(),
                     [this](const std::string& k) { return strat_mgr_.display_name(k); },
                     [this](const std::string& path, int bar_seconds_override,
                            const std::string& strat_key, double cash) {
                         TickLog log;
                         std::string err;
                         if (!tick_log_read(path, log, err)) {
                             route("replay: " + err);
                             return;
                         }
                         ReplayConfig cfg;
                         cfg.name = "replay:" +
                                    std::filesystem::path(path).filename().string();
                         cfg.log = std::move(log);
                         cfg.bar_seconds_override = bar_seconds_override;
                         cfg.initial_cash = cash;
                         // Same realistic order latency as live-sim sessions, so
                         // a replayed scalp fills the way it would have live
                         // (measured submit->ack latency, else the VPS default).
                         {
                             const auto [lat, jit] = sim_exec_latency(cfg_);
                             cfg.exec.latency_ns = lat;
                             cfg.exec.latency_jitter_ns = jit;
                         }
                         cfg.params = strat_mgr_.param_values(strat_key);
                         IStrategy* strat = acquire_strategy(strat_key);
                         if (!strat) {
                             route("replay: strategy '" + strat_key + "' is not loaded");
                             return;
                         }
                         if (engine_.start_replay(std::move(cfg), strat)) {
                             // Replay shares the backtest engine slot/flag.
                             leases_.push_back({strat, strat_key, StrategyLease::Backtest});
                             route("replay: running " + path + " (" +
                                      strat_mgr_.display_name(strat_key) + ")");
                         } else {
                             release_strategy({strat, strat_key, StrategyLease::Backtest});
                             route("replay: engine busy, try again");
                         }
                     });
    if (show_sweep_)
        sweep_panel_.draw(&show_sweep_, strat_mgr_.loaded_keys(),
                          [this](const std::string& k) { return strat_mgr_.display_name(k); },
                          [this](const std::string& k) { return strat_mgr_.param_values(k); },
                          sweep_,
                          [this](const SweepPanel::Request& rq) { queue_sweep(rq); },
                          [this](const SweepPanel::Request& rq) {
                              // Champion applies to this symbol's Trade tab.
                              start_tournament(rq, rq.symbol);
                          },
                          [this] {
                              sweep_.running = false;
                              // Drop the outstanding candle request and the
                              // pending per-symbol write: a cancelled run must
                              // not come back to life on a late batch, nor leave
                              // a half-validated fit behind.
                              cancel_pending_sweep();
                              sweep_fit_symbol_.clear();
                              sweep_fit_params_.clear();
                              if (tourn_.active) {
                                  tourn_.active = false;
                                  tourn_.for_lineup = false;
                                  sweep_.tourney.active = false;
                                  route("tournament: cancelled" +
                                        (tourn_.target_symbol.empty()
                                             ? std::string()
                                             : " - " + tourn_.target_symbol +
                                                   " has no fit from this build"));
                              } else {
                                  route("sweep: cancelled");
                              }
                          });
    if (show_strategy_) strat_mgr_.draw(&show_strategy_);
    if (show_build_output_) strat_mgr_.draw_build_output(&show_build_output_);
    if (show_trade_)
        trade_.draw(&show_trade_, strat_mgr_.loaded_keys(), param_specs_fn(),
                    [this](const std::string& k) { return strat_mgr_.display_name(k); },
                    [this](const std::string& sym) {
                        // Auto-pick: tournament on this symbol with the Optimizer
                        // panel's data settings; champion lands back in the tab.
                        const SweepPanel::Settings s = sweep_panel_.settings();
                        static constexpr const char* kRng[] = {"1mo", "6mo", "1y",
                                                               "2y",  "5y",  "max"};
                        SweepPanel::Request rq;
                        rq.symbol = sym;
                        rq.interval = sweep_interval_str(s.interval_idx);
                        rq.range = kRng[std::clamp(s.range_idx, 0, 5)];
                        rq.cash = s.cash;
                        rq.metric = s.metric;
                        rq.holdout_pct = s.holdout ? s.holdout_pct : 25.0;
                        start_tournament(rq, sym);
                    },
                    !polygon_key().empty(), !finnhub_key().empty(), data_.connected(),
                    trade_account_info(),
                    [this](const TradePanel::StartOpts& opts) { start_live_session(opts); },
                    [this](const std::string& sym) {   // row click -> chart the symbol
                        chart_.show_symbol(sym);
                        backtest_.set_symbol(sym);
                        show_chart_ = true;
                    });
    if (show_blotter_) blotter_.draw(&show_blotter_);
    if (show_positions_) positions_.draw(&show_positions_);
    if (show_journal_) journal_panel_.draw(&show_journal_);
    // Remember which of the two logs was in front (Begin reports false for a
    // docked tab sitting behind another), then restore it once on startup —
    // see Config::active_log for why ImGui's own tab memory isn't enough.
    const bool log_front = show_log_ && log_.draw("Log Console", &show_log_);
    const bool opt_front = show_opt_log_ && opt_log_.draw("Optimizer Log", &show_opt_log_);
    if (log_front) cfg_.active_log = "log";
    else if (opt_front) cfg_.active_log = "optlog";
    if (log_focus_frames_ <= 2 && !cfg_.active_log.empty()) {
        // Wait for frame 2: the windows must have been submitted once for their
        // dock tabs to exist, or SetWindowFocus has nothing to select. Runs
        // once, so it never fights the user changing tabs afterwards.
        if (++log_focus_frames_ == 2) {
            if (cfg_.active_log == "optlog" && show_opt_log_)
                ImGui::SetWindowFocus("Optimizer Log");
            else if (cfg_.active_log == "log" && show_log_)
                ImGui::SetWindowFocus("Log Console");
        }
    }

    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::route(std::string line) {
    // Send app-emitted optimizer/tournament/lineup/autopilot summaries to the
    // optimizer panel instead of the live console + /logs ring; everything else
    // (feed, broker, live, system) stays in the live log. The engine's backtest
    // strategy-log flood carries no prefix and is routed separately, by
    // engine_.running(), where it's drained (see pop_log loop).
    bool opt = false;
    static constexpr const char* kOpt[] = {"optimizer:", "tournament:", "autopilot:",
                                           "sweep:", "backtest", "lineup:"};
    for (const char* p : kOpt)
        if (line.rfind(p, 0) == 0) { opt = true; break; }
    LogConsole& sink = opt ? opt_log_ : log_;
    sink.add(std::move(line));
}

void App::route_operator(std::string line) {
    alert_scan(line);       // classify_alert matches "lineup: ABORTED"/"EXCLUDED"
    log_.add(std::move(line));   // the ring /logs and /events actually serve
}

TradePanel::ParamSpecsFn App::param_specs_fn() {
    return [this](const std::string& k) {
        std::vector<TradePanel::StratParam> out;
        for (const auto& s : strat_mgr_.param_specs(k))
            out.push_back({s.name, s.value, s.min, s.max});
        return out;
    };
}

// Cap on replayed history. Comfortably covers the largest lookback the sweep
// can elect (1000 bars) several times over, while keeping the replay to a few
// tens of milliseconds per session start.
static constexpr size_t kMaxSeedBars = 4000;

// Same interval strings the optimizer/lineup fetch under, so the cache the
// autopilot already fills every cycle is the cache we read here.
static const char* warmup_interval(int bar_sec) {
    return bar_sec >= 86400 ? "1d" : (bar_sec >= 3600 ? "1h" : "5m");
}

std::vector<tt::Bar> App::seed_bars(const std::string& symbol, int bar_sec) const {
    const char* ivl = warmup_interval(bar_sec);
    SeriesStore::Series s;
    uint64_t seen = 0;   // 0 = "unseen", so this copies whatever is cached
    if (!series_.copy_if_newer(symbol, ivl, seen, s) || s.candles.empty()) return {};
    const size_t n = std::min(s.candles.size(), kMaxSeedBars);
    std::vector<tt::Bar> out;
    out.reserve(n);
    for (size_t i = s.candles.size() - n; i < s.candles.size(); ++i) {
        const Candle& c = s.candles[i];
        out.push_back(tt::Bar{c.ts * 1'000'000'000LL, c.open, c.high, c.low, c.close, c.volume});
    }
    return out;
}

void App::start_live_session(const TradePanel::StartOpts& opts_in) {
    // The feed subscribes in symbol order, and IBKR's tick-by-tick cap means
    // the tail of the list falls back to sampled quotes, so order by what each
    // symbol's strategy actually needs off the tape (see net/feed_order.h).
    //
    // Sort the INPUT, once, here. The session's symbol ids are positions in
    // this list, and everything below — strategies, per-symbol params, risk
    // limits, accounts, bar sizes — is built by iterating it. 0.9.0 sorted a
    // separate copy and left the other loops on the original, so cfg.symbols
    // was reordered while cfg.symbol_params and the strategy vector were not:
    // AMIX ran ORB with RSI-2's parameters and SNDQ ran RSI-2 with ORB's.
    // Reordering the one list every loop reads makes that impossible.
    // TT_AUTORUN_LINEUP=1 is a PROPOSE-ONLY dry run: build the lineup, report
    // it, trade nothing. Enforced here rather than at the call sites because
    // this is the single choke point every start path goes through — the Trade
    // panel's Start button, its scheduled auto-start, the daily lineup's own
    // auto-start, and pump_lineup_swap's restart. Forcing it here means the
    // guarantee holds REGARDLESS of what the persisted config says about
    // lineup_propose_only or the session schedule, which is the whole point: a
    // dry run on the production box must not be able to trade because
    // config.json happens to be set to auto-start.
    if (dry_.active) {
        route_operator(std::string(kDryRunTag) +
                       " REFUSED a live-session start - TT_AUTORUN_LINEUP is a "
                       "propose-only dry run; no session is started and no order "
                       "is ever submitted");
        return;
    }
    TradePanel::StartOpts opts = opts_in;
    // THE UNREACHABLE-TIME-STOP GATE, at the one choke point every start path
    // goes through (Start button, scheduled auto-start, daily lineup, swap
    // restart). The lineup's own admission gate catches the scheduled path, but
    // a hand-added tab never goes through it and the operator's Start button
    // never has — and the parameter set that lost $506 on 2026-08-14 would be
    // just as unreachable arriving by either of those routes.
    //
    // REFUSE the symbol, START THE REST. A session of four healthy symbols is
    // strictly better than no session, and it is also better than five with one
    // that cannot close what it opens.
    //
    // The broker carve-out: a symbol the broker's last audit says we are holding
    // is kept, because dropping it here is not neutral — outside cfg.symbols it
    // is never adopted, never audited, and neither flatten() nor the 15:57
    // backstop can reach it (engine/reconcile_policy.h). Refusing to trade it
    // must not turn it into the orphan this branch exists to avoid.
    {
        std::vector<TradePanel::SymbolOpt> keep;
        std::vector<std::string> refused, kept_exposed;
        for (auto& so : opts.symbols) {
            if (time_stop_reachable(so.params, so.bar_seconds,
                                    so.risk.disable_auto_halt)) {
                keep.push_back(std::move(so));
                continue;
            }
            // broker_held_last_session, NOT broker_holds: this runs while no
            // session is live, so stand_down() has already cleared the live
            // book and broker_holds() would answer `unknown` every single time —
            // making the `|| unknown` carve-out below unconditional and this
            // whole gate incapable of refusing anything.
            bool unknown = false;
            if (book_audit_.broker_held_last_session(so.symbol, unknown) || unknown) {
                kept_exposed.push_back(so.symbol);
                keep.push_back(std::move(so));
                continue;
            }
            refused.push_back(so.symbol);
        }
        if (!kept_exposed.empty()) {
            std::string list;
            for (const std::string& s : kept_exposed)
                list += (list.empty() ? "" : ", ") + s;
            route_operator(std::string("live: ") + kUnreachableStopTag + " on " + list +
                           " - KEPT in the session because the broker book is not "
                           "confirmed flat there, so dropping them would orphan a real "
                           "position outside anything that can close it. They can "
                           "re-enter on that set once flat. Refit them.");
        }
        if (!refused.empty()) {
            std::string list;
            for (const std::string& s : refused)
                list += (list.empty() ? "" : ", ") + s;
            route_operator(std::string("live: REFUSED ") + list + " - " +
                           kUnreachableStopTag +
                           ": longer than a trading day, and these strategies place no "
                           "price stop, so nothing would ever close a losing position "
                           "(2026-08-14 STKH: -$506 in 2h16m). Starting the session "
                           "without them.");
        }
        opts.symbols = std::move(keep);
        if (opts.symbols.empty()) {
            route_operator(std::string("live: not starting - every symbol has an ") +
                           kUnreachableStopTag + ". Refit them and start again.");
            return;
        }
    }
    order_by_feed_fidelity(opts.symbols,
                           [](const TradePanel::SymbolOpt& s) { return s.strat_key; });

    std::vector<std::string> syms;
    std::vector<int> sym_bars;
    std::vector<std::string> sym_accts;
    std::vector<RiskLimits> sym_risk;
    std::vector<uint8_t> sym_hold_only;   // see LiveConfig::symbol_hold_only
    bool any_record = false;
    // Session-level equity/stale halts run on one portfolio, so
    // drive them from the tightest (min non-zero) per-symbol value.
    RiskLimits session_risk{};
    session_risk.daily_max_loss = 0;
    session_risk.max_drawdown_pct = 0;
    session_risk.stale_feed_sec = 0;
    auto tight = [](double cur, double v) {
        return v > 0 && (cur == 0 || v < cur) ? v : cur;
    };
    for (const auto& so : opts.symbols) {
        syms.push_back(so.symbol);
        sym_bars.push_back(so.bar_seconds);
        sym_accts.push_back(so.account);
        sym_risk.push_back(so.risk);
        sym_hold_only.push_back(so.hold_only ? 1 : 0);
        any_record = any_record || so.record;
        // A "hold — don't halt" symbol is left out of the equity-halt
        // aggregation, so its loss can't arm the session's auto-flatten; its
        // positions ride until the strategy exits or they recover. The notional
        // cap (below) and the stale-feed guard still apply.
        if (!so.risk.disable_auto_halt) {
            session_risk.daily_max_loss =
                tight(session_risk.daily_max_loss, so.risk.daily_max_loss);
            session_risk.max_drawdown_pct = tight(
                session_risk.max_drawdown_pct, so.risk.max_drawdown_pct);
        }
        if (so.risk.stale_feed_sec > 0 &&
            (session_risk.stale_feed_sec == 0 ||
             so.risk.stale_feed_sec < session_risk.stale_feed_sec))
            session_risk.stale_feed_sec = so.risk.stale_feed_sec;
    }
    // Per-trade notional guardrail: cap each position's dollar size so one
    // adverse move can't spend the whole day's loss budget in a single trade.
    // Strategies size off the ~$1M paper equity (risk_pct * cash), which dwarfs
    // the loss limit; the share caps are notional-blind and miss it. The engine
    // down-sizes orders to fit, and hands the number to the strategy up front
    // through ctx.budget().
    //
    // The derivation moved to tt::ui::position_notional_cap so the OPTIMIZER can
    // apply the identical number (see sweep_risk_limits). While it was inline
    // here, the replay had no cap at all and sized ~19x what this line permits.
    for (RiskLimits& rl : sym_risk)
        rl.max_position_notional = position_notional_cap(rl, opts.session_cash);
    // Sizing is invisible otherwise: the strategy asks for a qty, the engine
    // may shrink it, and nothing says what the ceiling was. Name it per symbol
    // so a "why is this position so small" question has an answer in the log.
    for (size_t i = 0; i < syms.size(); ++i) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "live: %s position budget $%.0f%s",
                      syms[i].c_str(), sym_risk[i].max_position_notional,
                      sym_risk[i].daily_max_loss > 0.0 ? "" : " (no daily-loss limit set)");
        route(buf);
    }

    // Session default bar size (feed gap-backfill granularity);
    // each symbol still aggregates at its own size below.
    const int session_bar =
        opts.symbols.empty() ? 60 : opts.symbols.front().bar_seconds;
    LiveConfig cfg;
    cfg.symbols = syms;
    cfg.initial_cash = opts.session_cash;
    // Session-level params stay empty: every symbol carries its
    // own map in symbol_params (ctx.param resolves per symbol).
    cfg.bar_seconds = session_bar;
    cfg.symbol_bar_seconds = sym_bars;
    cfg.risk = session_risk;
    cfg.symbol_risk = sym_risk;
    cfg.symbol_hold_only = sym_hold_only;
    // THE ENTRY GATE. Installed for every live session the app starts, so no
    // strategy can open a position outside the hours in which one can be closed
    // again — see LiveConfig::entry_gate and tt::rth_entry_allowed. The engine
    // defaults it OFF because backtests and replays must keep judging the data's
    // clock, not the world's; a live session that reads entry_gate.armed false
    // in /diag is therefore a bug HERE, not there.
    cfg.entry_gate = [](int64_t now_ns) { return rth_entry_allowed_at(now_ns); };
    // THE SAME CALENDAR, HANDED TO THE OTHER END OF THE DAY. entry_cutoff_h is
    // already "the last minute a position may be OPENED because it is the last
    // minute something will still CLOSE it" — so it is, by construction, the hour
    // the backstop must close ON. Sharing the one function keeps the two ends
    // from drifting apart again: they were 2 h 57 m apart on an early close, the
    // gate calendar-aware and the backstop pinned to a literal 15.95.
    cfg.eod_flatten_h_for_day = [](const std::tm& tm) { return entry_cutoff_h(tm); };
    // Every data source is real-time now => spin the engine
    // thread; ticks are handled in ns, not after Sleep(5).
    cfg.busy_spin = true;
    // Optional core pinning (TT_PIN_ENGINE / TT_PIN_FEED =
    // core index): kills scheduler-migration jitter.
    if (const char* pin = std::getenv("TT_PIN_ENGINE"))
        cfg.pin_core = std::atoi(pin);
    // Simulator fills model the real IBKR order path: the
    // live-measured submit->ack latency once enough acks are
    // in, else the VPS default (~15 ms RTT + gateway/backend
    // processing => ~75 ms). Matters for scalping; negligible
    // for bar-scale strategies.
    {
        const auto [lat, jit] = sim_exec_latency(cfg_);
        cfg.exec.latency_ns = lat;
        cfg.exec.latency_jitter_ns = jit;
    }
    // Per-strategy callback watchdog (huge headroom over the
    // µs a normal callback takes; catches runaways only).
    cfg.watchdog_ms = 250;
    if (const char* w = std::getenv("TT_WATCHDOG_MS"))
        cfg.watchdog_ms = std::atoi(w);
    if (any_record) {
        std::error_code ec;
        std::filesystem::create_directories(sessions_dir(), ec);
        char name[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(name, sizeof name, "%Y%m%d_%H%M%S.ttk", &tm);
        cfg.capture_path = sessions_dir() + "\\" + name;
    }
    std::unique_ptr<IbkrBroker> ibkr_broker;
    std::unique_ptr<TwsBroker> tws_broker;
    if (opts.broker == TradePanel::Broker::Ibkr) {
        IbkrConfig ic;
        if (const char* gw = std::getenv("TT_IBKR_GATEWAY"))
            ic.gateway_url = gw;
        ic.symbols = syms;
        ic.symbol_accounts = sym_accts;   // per-symbol sub-account routing
        ic.read_only = read_ibkr_accounts().active_readonly();
        ibkr_broker = std::make_unique<IbkrBroker>(std::move(ic));
        cfg.broker = ibkr_broker.get();
        route(ic.read_only
                     ? "live: IBKR account is READ-ONLY — orders blocked"
                     : "live: routing orders to IBKR gateway");
    } else if (opts.broker == TradePanel::Broker::Tws) {
        TwsConfig tc;
        tc.port = tws_api_port();
        tc.symbols = syms;
        tc.symbol_accounts = sym_accts;
        tc.read_only = read_ibkr_accounts().active_readonly();
        // FIXED, forever. Not rotated, not configurable — see
        // engine/tws_client_id.h for the full 2026-08-13 post-mortem.
        //
        // The one-line version: IB scopes an order's orderStatus, execDetails
        // and commissionReport to the client id that PLACED it, while
        // reqAllOpenOrders lets ANY client see every resting order in the
        // account. Rotating the id therefore produced sessions that adopted
        // orders they could neither hear nor cancel. On 2026-08-13 a lineup swap
        // moved 20 -> 22, client 22 adopted RAM's take-profit, the take-profit
        // filled, and the app carried a phantom 411-share position for 4 h 03 m
        // while reporting a fictional +$528 and "protected".
        //
        // What rotation bought — an automatic escape from "error 326: client id
        // already in use" — is now bought instead by a tiered retry plus a
        // delayed page (tws_client_id_retry_sec, kTwsClientIdPageAfterSec). That
        // trade is deliberate: 326 is a nuisance a human can clear, and adopting
        // an order the app is deaf to is not.
        tc.client_id = kTwsOrdersClientId;
        const int port = tc.port;
        const int cid = tc.client_id;
        // Reap any leftover broker BEFORE constructing the new one: TwsBroker's
        // constructor spawns its I/O thread and connects immediately, so with a
        // fixed client id an old adapter still holding id 20 is a guaranteed
        // self-collision. The normal path (safe_stop_live) has already reaped;
        // this covers Start-without-Stop, and used to sit 200 lines further
        // down — after the new broker was already dialling.
        reap_async(std::move(tws_), broker_reaps_);
        reap_async(std::move(ibkr_), broker_reaps_);
        // ...and WAIT for it. Moving the reap up was not enough on its own:
        // reap_async DETACHES a thread and returns immediately, so the very next
        // statement dialled client 20 while the old adapter still held it —
        // ~TwsBroker deliberately does not poke the reader signal (see
        // tws_broker.cpp), so its I/O thread only notices stop_ when
        // waitForSignal times out, up to a second later. Only the lineup swap
        // waited; the direct-start path did not, and the scheduled 15:55 stop
        // goes through Engine::stop_live rather than App::safe_stop_live, so
        // tws_ survives the night and 09:25's auto-start hit exactly this.
        wait_for_broker_reap("live");
        tws_broker = std::make_unique<TwsBroker>(std::move(tc));
        cfg.broker = tws_broker.get();
        route("live: routing orders via TWS socket (port " + std::to_string(port) +
                 ", client " + std::to_string(cid) + ")");
    }
    // Each symbol needs its strategy built + loaded first.
    std::string unbuilt;
    for (const auto& so : opts.symbols)
        if (!so.strat_key.empty() &&
            !strat_mgr_.loaded_fresh(so.strat_key)) {
            strat_mgr_.request_load(so.strat_key);
            unbuilt += (unbuilt.empty() ? "" : ", ") + so.strat_key;
        }
    if (!unbuilt.empty()) {
        route("live: building strategies (" + unbuilt +
                 ") — click Start Trading again");
        return;
    }
    // One strategy instance + param set per symbol.
    std::vector<IStrategy*> strategies;
    std::vector<StrategyLease> new_leases;
    // symbol -> "own" / "mixed" / "inherited" / "none", published to /diag once
    // the session actually starts (see symbol_params.h).
    std::map<std::string, std::string> param_src;
    // Symbols whose warmup history was not in the cache, with the engine symbol
    // id to reseed. Local, not warmup_want_: this list is built while the session
    // is still only a plan, and start_live can still fail below.
    std::vector<WarmupWant> cold_warmup;
    bool acq_ok = true;
    for (const auto& so : opts.symbols) {
        IStrategy* inst = acquire_strategy(so.strat_key);
        if (!inst) {
            route("live: strategy '" +
                     strat_mgr_.display_name(so.strat_key) +
                     "' failed to load");
            acq_ok = false;
            break;
        }
        strategies.push_back(inst);
        // build_start_opts already resolved this symbol's set (own value per
        // declared name, strategy default for the rest) and recorded WHICH.
        // Say it out loud: on 2026-08-10 six symbols silently started on one
        // borrowed set and nothing in the log or /diag named the inheritance.
        std::map<std::string, double> sp = so.params;
        param_src[so.symbol] = param_source_name(so.param_source);
        if (so.param_source == ParamSource::Inherited ||
            so.param_source == ParamSource::Mixed) {
            std::string names;
            for (const std::string& n : so.inherited_params)
                names += (names.empty() ? "" : ", ") + n;
            // State only what is checkable. The first draft asserted these
            // values "came from the <strategy> defaults", but all the code knows
            // is that they came from the SHARED map — which the Optimizer panel
            // writes deliberately, and which every pre-0.16.0 build let each
            // tournament candidate scribble into. Whose fit is in there is
            // exactly the thing nobody can tell, and that is the point.
            route("live: " + so.symbol + " params " +
                     std::string(param_source_name(so.param_source)) +
                     " - " + names + " came from the shared " +
                     strat_mgr_.display_name(so.strat_key) +
                     " parameter map, NOT from a fit on " + so.symbol);
        } else if (so.param_source == ParamSource::Own) {
            route("live: " + so.symbol + " params own (fitted to " + so.symbol + ")");
        }
        // "hold — don't halt" mode: tell hold-aware strategies not to force-flatten
        // an underwater position in their EOD/new-day housekeeping (they read
        // ctx.param("__hold_losers")). Live-only overlay — backtests behave normally.
        if (so.risk.disable_auto_halt) sp["__hold_losers"] = 1.0;
        cfg.symbol_params.push_back(std::move(sp));
        // Warm this instance on cached history. Without it the strategy starts
        // with zero bars and a lookback of a few hundred can never be satisfied
        // inside one session — the daily lineup swap re-inits every morning.
        std::vector<tt::Bar> seed = seed_bars(so.symbol, cfg.bar_seconds);
        if (seed.empty()) {
            // Cold cache (always true for the first session after launch, since
            // series_ lives only in memory). Fetch it and re-seed on arrival.
            // "1mo" of 5m bars is ~1,640 - comfortably more than the largest
            // lookback the sweep can elect, and deliberately under the ~3,000-bar
            // batch size that makes IB drop the NEXT request (see hist_pacing.h).
            //
            // Only noted here; the fetch (and the ticket that claims its
            // delivery) is issued below, once start_live has actually succeeded —
            // a request id cannot be recorded before the request is made.
            cold_warmup.push_back(
                {so.symbol, static_cast<uint32_t>(cfg.symbol_warmup.size() + 1)});
        }
        cfg.symbol_warmup.push_back(std::move(seed));
        new_leases.push_back({inst, so.strat_key, StrategyLease::Live});
    }
    if (!acq_ok) {
        for (const auto& l : new_leases) release_strategy(l);
        return;
    }
    // Everything above is built by index off one list, so a params/strategy
    // mismatch is silent — the engine cannot tell that a set belongs to a
    // different symbol, and ctx.param() just returns the fallback for every
    // name the strategy asks for. 0.9.0 shipped exactly that. Catch it here:
    // a param the strategy never declared can only have come from another
    // symbol's set. Cheap, runs once per session, and names the symbol.
    for (size_t i = 0; i < cfg.symbol_params.size() && i < opts.symbols.size(); ++i) {
        const auto& so = opts.symbols[i];
        const auto specs = strat_mgr_.param_specs(so.strat_key);
        for (const auto& [name, val] : cfg.symbol_params[i]) {
            if (name.rfind("__", 0) == 0) continue;   // engine overlays (__hold_losers)
            const bool declared =
                std::any_of(specs.begin(), specs.end(),
                            [&](const auto& s) { return name == s.name; });
            if (!declared)
                route("live: WARNING " + so.symbol + " carries parameter '" + name +
                      "' which " + strat_mgr_.display_name(so.strat_key) +
                      " does not declare — parameter sets may be mismatched");
        }
    }
    const int warm_bar_sec = cfg.bar_seconds;
    if (engine_.start_live(std::move(cfg), std::move(strategies))) {
        live_param_source_ = std::move(param_src);
        for (const auto& l : new_leases) leases_.push_back(l);
        // Pull history for any symbol that started cold; on_candles re-seeds it.
        // ReqPriority::Live, not Bulk: an unseeded strategy cannot satisfy a
        // several-hundred-bar lookback inside one session, so these six requests
        // are the ones that must never queue behind a lineup's thirty ranking
        // fetches (see net/hist_pacing.h). Under the lock across every call, so
        // no delivery can arrive before the id that claims it is on record.
        {
            std::lock_guard lock(warmup_mu_);
            warmup_want_.clear();   // a previous session's tickets are not ours
            for (const WarmupWant& w : cold_warmup) {
                const uint32_t rid =
                    data_.request_candles(w.symbol, warmup_interval(warm_bar_sec),
                                          "1mo", net::ReqPriority::Live);
                if (rid) warmup_want_[rid] = w;
                else
                    route("live: WARNING " + w.symbol +
                          " starts cold and the feed would not accept its warmup "
                          "request - it will trade on tick-aggregated bars only");
            }
            if (!warmup_want_.empty())
                route("live: fetching warmup history for " +
                         std::to_string(warmup_want_.size()) + " cold symbol(s)");
        }
        // Arm the autopilot for symbols that asked for it.
        ap_ = Autopilot{};
        for (size_t i = 0; i < opts.symbols.size(); ++i) {
            const auto& so = opts.symbols[i];
            if (so.ap_mode <= 0) continue;
            Autopilot::Sym aps;
            aps.symbol = so.symbol;
            aps.sid = static_cast<uint32_t>(i + 1);
            aps.mode = so.ap_mode;
            aps.trigger = so.ap_trigger;
            aps.interval_min = so.ap_interval_min;
            aps.dd_pct = so.ap_dd_pct;
            aps.key = so.strat_key;
            aps.last_cycle_s = mono_s();
            ap_.syms.push_back(std::move(aps));
        }
        if (!ap_.syms.empty())
            route("autopilot: armed for " +
                     std::to_string(ap_.syms.size()) + " symbol(s)");
        // The engine's live thread was joined inside start_live,
        // so it's safe to drop whatever the previous session left
        // here. Normally safe_stop_live() reaped these already, and
        // the TWS branch above reaps again before it constructs the
        // new adapter (it must: a fixed client id cannot share the
        // gateway with a leftover holding it). This is what is left
        // of that defense in depth — a route that built no broker.
        // TWS's connect call is blocking, so any of these COULD be
        // mid-reconnect; never destroy them synchronously here.
        reap_async(std::move(ibkr_), broker_reaps_);
        reap_async(std::move(tws_), broker_reaps_);
        ibkr_ = std::move(ibkr_broker);
        tws_ = std::move(tws_broker);
        reap_async(std::move(polygon_feed_));   // previous session's feeds
        reap_async(std::move(finnhub_feed_));
        reap_async(std::move(ibkr_feed_));
        reap_async(std::move(tws_feed_));
        rt_feed_active_.store(false, std::memory_order_relaxed);
        const auto sink = [this](const EngineEvent& ev) {
            return engine_.push_feed_event(ev);
        };
        if (opts.data == TradePanel::DataFeed::Polygon &&
            !polygon_key().empty()) {
            PolygonFeedConfig pc;
            pc.api_key = polygon_key();
            // e.g. wss://delayed.polygon.io/stocks to test the
            // adapter on the $29 delayed tier (same protocol).
            if (const char* ws = std::getenv("TT_POLYGON_WS"))
                pc.stream_url = ws;
            pc.symbols = syms;
            pc.busy_poll = std::getenv("TT_FEED_SPIN") != nullptr;
            if (const char* pin = std::getenv("TT_PIN_FEED"))
                pc.pin_core = std::atoi(pin);
            pc.bar_seconds = session_bar;
            polygon_feed_ =
                std::make_unique<PolygonFeed>(std::move(pc), sink);
            polygon_feed_->start();
            rt_feed_active_.store(true, std::memory_order_relaxed);
        } else if (opts.data == TradePanel::DataFeed::Tws) {
            TwsFeedConfig fc;
            fc.port = tws_api_port();
            fc.symbols = syms;
            // The FEED id still rotates (40-59), on its own counter now that the
            // broker no longer shares one. Rotation is safe here for the reason
            // it was never safe on the order path: the feed places no orders, so
            // nothing is scoped to its id and it has nothing to adopt. Disjoint
            // from the broker (kTwsOrdersClientId = 20) and data (9).
            fc.client_id = tws_feed_client_id(tws_feed_seq_++);
            tws_feed_ =
                std::make_unique<TwsFeed>(std::move(fc), sink);
            tws_feed_->start();
            rt_feed_active_.store(true, std::memory_order_relaxed);
        } else if (opts.data == TradePanel::DataFeed::Finnhub &&
                   !finnhub_key().empty()) {
            FinnhubFeedConfig fc;
            fc.api_key = finnhub_key();
            if (const char* ws = std::getenv("TT_FINNHUB_WS"))
                fc.stream_url = ws;
            fc.symbols = syms;
            fc.busy_poll = std::getenv("TT_FEED_SPIN") != nullptr;
            if (const char* pin = std::getenv("TT_PIN_FEED"))
                fc.pin_core = std::atoi(pin);
            fc.bar_seconds = session_bar;
            finnhub_feed_ =
                std::make_unique<FinnhubFeed>(std::move(fc), sink);
            finnhub_feed_->start();
            rt_feed_active_.store(true, std::memory_order_relaxed);
        } else if (opts.data == TradePanel::DataFeed::Ibkr) {
            IbkrFeedConfig fc;
            if (const char* gw = std::getenv("TT_IBKR_GATEWAY")) {
                fc.gateway_url = gw;
                // wss://host/v1/api/ws mirrors the REST base.
                std::string ws = fc.gateway_url;
                if (ws.rfind("https://", 0) == 0)
                    ws.replace(0, 8, "wss://");
                fc.ws_url = ws + "/ws";
            }
            fc.symbols = syms;
            fc.bar_seconds = session_bar;
            if (const char* pin = std::getenv("TT_PIN_FEED"))
                fc.pin_core = std::atoi(pin);
            ibkr_feed_ =
                std::make_unique<IbkrFeed>(std::move(fc), sink);
            ibkr_feed_->start();
            rt_feed_active_.store(true, std::memory_order_relaxed);
        }
        for (const std::string& sym : syms)
            watchlist_.ensure(sym);  // quote subscription feeds the engine
        // Log each symbol with the strategy it runs.
        std::string joined;
        for (const auto& so : opts.symbols)
            joined += (joined.empty() ? "" : ", ") + so.symbol + ":" +
                      strat_mgr_.display_name(so.strat_key);
        route("live: session queued for " + joined);
    } else {
        for (const auto& l : new_leases) release_strategy(l);
        route("live: cannot start (engine busy)");
    }
}

void App::request_quit() {
    if (engine_.live_running()) pending_quit_ = true;   // draw_trading_guards() confirms
    else should_quit_ = true;
}

// Block until every broker adapter handed to reap_async has actually been
// destroyed, or kSwapReapWaitS elapses.
//
// The only bounded wait that is honest here: it waits on a fact this process
// OWNS and can observe — the destructor returned — not on a guess about how long
// the gateway holds a released client id. The lineup swap does the same thing
// across frames (pump_lineup_swap) because it can; start_live_session cannot,
// so it blocks. That is deliberate: the cost is a stall of at most one gateway
// teardown (~1 s, and only when there was a leftover adapter), against dialling
// kTwsOrdersClientId while this process still holds it, which costs a
// self-inflicted "error 326: client id already in use" and an order path that is
// unavailable for the first fast-retry cycle of the session.
void App::wait_for_broker_reap(const char* who) {
    if (broker_reaps_->load(std::memory_order_acquire) == 0) return;
    const double deadline = mono_s() + kSwapReapWaitS;
    while (broker_reaps_->load(std::memory_order_acquire) > 0 && mono_s() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Past the deadline it proceeds anyway rather than refusing to start: the
    // 326 tier (fast retries, page only after a minute) is the backstop, and a
    // session that never comes up is worse than one that retries for a moment.
    if (broker_reaps_->load(std::memory_order_acquire) > 0)
        route(std::string(who) + ": previous broker still shutting down after " +
              std::to_string(static_cast<int>(kSwapReapWaitS)) +
              "s - starting anyway (expect a brief client-id retry)");
}

void App::safe_stop_live(bool keep_positions) {
    if (!engine_.live_running()) return;
    // keep_positions only makes sense on a route that re-adopts them on the next
    // start (TWS reconciles()); on any other route a restart comes back flat and
    // blind, so flatten. When keeping, we skip the kill switch entirely: the
    // positions and their resting stop/TP orders stay live at the broker and are
    // adopted + held on restart (see run_live's reconciliation gate).
    const bool keep = keep_positions && tws_ && tws_->reconciles();
    if (!keep) engine_.kill_switch();   // cancel all + flatten + halt strategy
    // keep => graceful stop that LEAVES the resting broker orders live so the
    // restart re-adopts them (otherwise stop_live cancels them; the position
    // would come back naked + paused). Joins the live thread — after this,
    // nothing still references cfg.broker, so tearing the broker down below can
    // never race the engine.
    engine_.stop_live(keep);
    // Actually disconnect: leaving the broker connected after "Stop" holds its
    // TWS client_id at the gateway, so a quick Start can collide with it
    // (error 326) and stall retrying — reaped async so a stuck reconnect on
    // the OLD broker can never freeze this click (see reap_async).
    reap_async(std::move(tws_), broker_reaps_);
    reap_async(std::move(ibkr_), broker_reaps_);
    // The book auditor belongs to a SESSION, not the process: leaving it armed
    // would let an "ok" recorded before the stop stand as evidence about the
    // next session's book, and leaving it unarmed-but-stale would read Blind
    // forever with nothing to be blind about.
    book_audit_.stand_down();
    route(keep ? "account: stopped live trading (positions kept for restart)"
                  : "account: stopped live trading (kill switch)");
}

void App::do_ibkr_signout() {
    const std::string args = signout_args();
    if (!args.empty()) {
        run_hidden(args);
        route("account: signing out of IBKR (stopping auto-login, gateway logout)");
    } else {
        route("account: sign-out script missing (scripts\\Stop-IbkrLogin.ps1)");
    }
}

// Top-level confirm dialogs so signing out or quitting while a live trading
// session is running flattens + stops it first (same guard as switching).
void App::draw_trading_guards() {
    if (pending_signout_ && !ImGui::IsPopupOpen("Confirm sign out"))
        ImGui::OpenPopup("Confirm sign out");
    if (ImGui::BeginPopupModal("Confirm sign out", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Signing out will cancel "
                           "open orders, flatten positions, and stop it.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & sign out")) {
            safe_stop_live();
            do_ibkr_signout();
            pending_signout_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_signout_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (pending_quit_ && !ImGui::IsPopupOpen("Confirm quit"))
        ImGui::OpenPopup("Confirm quit");
    if (ImGui::BeginPopupModal("Confirm quit", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Quitting will cancel open "
                           "orders, flatten positions, and stop it before exit.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & quit")) {
            safe_stop_live();
            pending_quit_ = false;
            should_quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep running")) {
            pending_quit_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// A small window that surfaces only when the background check finds origin/main
// ahead of this build. "Restart & Update" runs the same flatten+stop safeguard
// as quitting, then hands off to the updater (pull + rebuild + relaunch).
void App::draw_update_panel() {
    const std::string remote = update_.remote_commit();
    const bool show = update_.available() && !remote.empty() &&
                      remote != update_dismissed_commit_;
    // On the reconciling (TWS) route we can restart WITHOUT flattening: positions
    // + resting orders stay at the broker and are re-adopted on restart. Any
    // other route (or no broker) comes back flat, so we flatten first.
    const bool keep_positions = engine_.live_running() && tws_ && tws_->reconciles();
    if (show && !pending_update_) {
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Update available", nullptr, ImGuiWindowFlags_NoCollapse |
                                                          ImGuiWindowFlags_NoDocking)) {
            ImGui::TextWrapped("A newer build is on GitHub (origin/main).");
            ImGui::Spacing();
            ImGui::Text("Running:  v%s", TT_VERSION_BASE);
            const std::string latest = update_.remote_version();
            if (!latest.empty())
                ImGui::Text("Latest:   v%s", latest.c_str());
            else
                ImGui::TextDisabled("Latest:   newer build available");
            ImGui::Spacing();
            if (engine_.live_running()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                if (keep_positions)
                    ImGui::TextWrapped("A live session is running. Open positions stay at the "
                                       "broker and are re-adopted on restart; the session stops.");
                else
                    ImGui::TextWrapped("A live session is running. Updating will cancel open "
                                       "orders, flatten positions, and stop it first.");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
            if (ImGui::Button("Restart & Update"))
                pending_update_ = true;
            ImGui::SameLine();
            if (ImGui::Button("Later"))
                update_dismissed_commit_ = remote;   // reappears when main moves again
        }
        ImGui::End();
    }

    if (pending_update_ && !ImGui::IsPopupOpen("Confirm update"))
        ImGui::OpenPopup("Confirm update");
    if (ImGui::BeginPopupModal("Confirm update", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (keep_positions)
            ImGui::TextWrapped("Open positions stay at the broker — protected only by their "
                               "resting stop/TP orders — while the app rebuilds (a few "
                               "minutes), then are re-adopted and held until flat on restart. "
                               "The live session stops, then it pulls + rebuilds and relaunches.\n\n"
                               "Note: if your IB Gateway is set to cancel orders on disconnect, "
                               "the position is unprotected while the app is down.");
        else if (engine_.live_running())
            ImGui::TextWrapped("This will cancel open orders, flatten positions, and stop "
                               "the live session, then pull + rebuild and relaunch the app.");
        else
            ImGui::TextWrapped("This will pull the latest main, rebuild, and relaunch the app.");
        ImGui::Spacing();
        if (ImGui::Button("Update & restart")) {
            safe_stop_live(keep_positions);   // keeps positions on the TWS route
            launch_updater();
            should_quit_ = true;
            pending_update_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_update_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Manual "Check for updates" (Help menu): poke the checker and let the
    // result surface directly — a newer build opens the Update panel above; the
    // up-to-date / error outcomes report to the log. No intermediary popup.
    if (update_check_click_) {
        update_check_click_ = false;
        update_check_wait_ = true;
        update_check_gen_ = update_.check_count();
        update_check_started_ = mono_s();
        update_dismissed_commit_.clear();   // an explicit check re-shows a dismissed update
        update_.check_now();
    }
    if (update_check_wait_) {
        const bool done = update_.check_count() != update_check_gen_;
        const bool timed_out = mono_s() - update_check_started_ > 20.0;
        if (done || timed_out) {
            update_check_wait_ = false;
            if (!done)
                route("update: check timed out — try again");
            else if (!update_.last_ok())
                route("update: couldn't reach GitHub — check the connection");
            else if (update_.available())
                route("update: a newer build is available (" + update_.remote_commit() +
                         ") — see the Update panel");
            else
                route(std::string("update: up to date (") + TT_VERSION + ")");
        }
    }
}

// Detach the updater script: it waits for THIS process to exit (releasing the
// single-instance mutex + any file locks), then git pull + cmake --build the
// release preset + relaunch this exe. Fire-and-forget; we quit right after.
void App::launch_updater() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string extra = "-WaitPid " + std::to_string(GetCurrentProcessId()) +
                              " -Exe \"" + std::string(exe) + "\"" +
                              " -Preset ucrt64-release";
    const std::string args = ps_args("Update-And-Restart.ps1", /*hidden=*/true, extra);
    if (args.empty()) {
        route("update: updater script missing (scripts\\Update-And-Restart.ps1)");
        return;
    }
    run_hidden(args);   // detached: keeps running after we exit
    route("update: launched updater (pull + rebuild + relaunch); quitting");
#else
    route("update: self-update is Windows-only");
#endif
}

void App::draw_account_menu() {
    if (!ImGui::BeginMenu("Account")) return;
    if (data_.connected()) {
        const std::string acct = data_.account();
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1), "ibkr: session active%s%s",
                           acct.empty() ? "" : ("  (" + acct + ")").c_str(),
                           use_tws_data_ ? "  [tws]" : "");
        badge_kind(data_.account_kind());   // PAPER (green) / LIVE (red)
        if (read_ibkr_accounts().active_readonly()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "READ-ONLY");
        }
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "ibkr: no gateway session");
    }
    ImGui::Separator();

    // Account switching lives in the dialog (pick an account, click it).
    // TWS route: the session belongs to IB Gateway, which Switch-IbkrAccount /
    // Stop-IbkrLogin (CP web scripts) know nothing about — hide the footguns.
    if (use_tws_data_) {
        ImGui::TextDisabled("TWS route: session is managed by IB Gateway");
        ImGui::TextDisabled("(switch: scripts\\Start-IbGateway.ps1 -Account <id>)");
    } else if (ImGui::MenuItem(data_.connected() ? "Switch" : "Sign In")) {
        signin_.account_request_open = true;
    }

    const auto ibkr = read_ibkr_accounts();

    if (ImGui::BeginMenu("Sign Out", data_.connected() && !use_tws_data_)) {
        const std::string acct = data_.account();
        const std::string label =
            "IBKR gateway" + (acct.empty() ? std::string() : ": " + acct);
        if (ImGui::MenuItem(label.c_str())) {
            if (engine_.live_running()) pending_signout_ = true;  // confirm + safe-stop
            else do_ibkr_signout();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Remove", !ibkr.accounts.empty())) {
        ImGui::TextDisabled("Deletes the saved credentials");
        for (const auto& acc : ibkr.accounts) {
            const std::string label = "IBKR: " + acc.label + "##" + acc.name;
            if (ImGui::MenuItem(label.c_str())) {
                const std::string args = ps_args("Remove-IbkrAccount.ps1", true,
                                                 "-Account \"" + acc.name + "\"");
                if (!args.empty()) {
                    run_hidden(args);
                    route("account: removing IBKR '" + acc.name + "'");
                }
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Data feeds (Polygon / Finnhub API keys) — separate from the broker Account
// menu. IBKR gateway data needs no key and is picked in the Trade panel.
void App::draw_data_menu() {
    if (!ImGui::BeginMenu("Data")) return;
    const std::string poly = accounts_.active_name("polygon");
    const std::string finn = accounts_.active_name("finnhub");
    ImGui::TextDisabled("polygon: %s", poly.empty() ? "(not signed in)" : poly.c_str());
    ImGui::TextDisabled("finnhub: %s", finn.empty() ? "(not signed in)" : finn.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem("Add Feed"))
        signin_.data_request_open = true;

    if (ImGui::BeginMenu("Sign Out", !poly.empty() || !finn.empty())) {
        if (!poly.empty() && ImGui::MenuItem(("polygon: " + poly).c_str())) {
            accounts_.sign_out("polygon");
            route("account: signed out of polygon");
        }
        if (!finn.empty() && ImGui::MenuItem(("finnhub: " + finn).c_str())) {
            accounts_.sign_out("finnhub");
            route("account: signed out of finnhub");
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Remove", !accounts_.list().empty())) {
        ImGui::TextDisabled("Deletes the saved credentials");
        for (const auto& e : accounts_.list()) {
            const std::string label = e.name + "  (" + e.provider + ")";
            if (ImGui::MenuItem(label.c_str())) {
                accounts_.remove(e.name);
                route("account: removed '" + e.name + "'");
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Route noteworthy engine/broker/feed log lines to the alert channel.
// Critical = money is at risk right now; Warning = something needs a look;
// Info = fills (webhook only, no beep — they can be frequent).
//
// Callable from ANY thread since 0.20.0: the market-data source's on_log fires
// on its own I/O thread and now goes through here (before that it was the one
// adapter whose lines never reached the alert channel). That is safe because
// classify_alert is pure, AlertNotifier::notify is mutex-guarded, and
// dry_.active is written once in the constructor — before any of those threads
// exists — and never again.
// Push the persisted Settings > Notifications switches into the notifier.
// Called at startup and on every toggle, so the live gate and config.json can
// never disagree — a category that LOOKS off in the menu but still pages (or
// worse, looks on and does not) is the kind of thing nobody notices until an
// alert they wanted never arrives.
void App::apply_notification_settings() {
    using C = AlertCategory;
    alerts_.set_category_enabled(static_cast<int>(C::Trades), cfg_.notify_trades);
    alerts_.set_category_enabled(static_cast<int>(C::Orders), cfg_.notify_orders);
    alerts_.set_category_enabled(static_cast<int>(C::Risk), cfg_.notify_risk);
    alerts_.set_category_enabled(static_cast<int>(C::Connection), cfg_.notify_connection);
    alerts_.set_category_enabled(static_cast<int>(C::Integrity), cfg_.notify_integrity);
    alerts_.set_category_enabled(static_cast<int>(C::Lineup), cfg_.notify_lineup);
    alerts_.set_category_enabled(static_cast<int>(C::System), cfg_.notify_system);
}

void App::alert_scan(const std::string& l) {
    // A dry run runs the REAL lineup, so it emits the real verdicts — including
    // "lineup: ABORTED", which classify_alert rates Critical and which would page
    // the operator as though the morning's production build had refused the
    // trading day. It didn't; a scheduled rehearsal did. The line still reaches
    // the log ring, /logs and /events, so nothing is hidden — only the phone is
    // spared. The dry run's own verdict travels in its exit code.
    if (dry_.active) return;
    switch (classify_alert(l)) {
    case AlertClass::Critical: alerts_.notify(AlertNotifier::Critical, l); break;
    case AlertClass::Warning:  alerts_.notify(AlertNotifier::Warning, l); break;
    case AlertClass::Info:     alerts_.notify(AlertNotifier::Info, l); break;
    case AlertClass::None:     break;
    }
}

// Broker-disconnect watchdog. alert_scan above only fires on lines the adapters
// actually log; a wedged/half-open gateway can go silent (see the overnight
// outages), so nothing would ever trip it. This checks broker readiness as
// STATE each frame instead: during a live session on a real broker, if the
// order path stays not-ready past kBrokerDownAlertSec, raise a critical alert
// (webhook), and re-raise every kBrokerDownReAlertSec while it's still down so a
// persistent outage keeps nagging. On recovery, one Warning that it's back.
//
// The grace/re-alert bookkeeping is WatchdogTimer's (watchdog_timer.h) — shared
// with pump_orphan_watchdog and, unlike this function, unit-testable. It runs on
// mono_s(): on ImGui::GetTime() an outage that began before the window was
// minimized simply stopped ageing, so the 60 s threshold was unreachable for as
// long as nobody was looking.
void App::pump_broker_watchdog() {
    static constexpr double kBrokerDownAlertSec = 60.0;
    static constexpr double kBrokerDownReAlertSec = 600.0;   // re-ping every 10 min while down
    const bool have_broker = (tws_ != nullptr) || (ibkr_ != nullptr);
    if (!engine_.live_running() || !have_broker) {   // nothing to guard (idle / sim broker)
        broker_wd_.reset();
        return;
    }
    // "Up" = the order path can actually reach IBKR: the local API socket is up
    // AND (TWS route) the gateway's upstream link to IBKR is up. Keying off the
    // socket alone missed a gateway that's up-but-disconnected-upstream (IBKR
    // maintenance / weekend reset). A transient nightly 1100->1102 blip clears
    // well inside the 60s grace, so only a SUSTAINED loss pages.
    const bool socket_ready = ibkr_ ? ibkr_->ready() : tws_->ready();
    const bool upstream_ok = tws_ ? tws_->upstream_connected() : true;
    const bool ready = socket_ready && upstream_ok;
    const double now = mono_s();
    switch (broker_wd_.update(!ready, now, kBrokerDownAlertSec, kBrokerDownReAlertSec)) {
    case WatchdogTimer::Action::None:
        return;
    case WatchdogTimer::Action::Recovered:
        alerts_.notify(AlertNotifier::Warning, "broker: reconnected to IBKR");
        return;
    case WatchdogTimer::Action::Page:
        break;
    }
    const double down_s = broker_wd_.age_s(now);   // a Page leaves the episode open
    // Distinguish the two failure modes so the alert points at the right thing.
    const char* what = socket_ready
                           ? "gateway lost its connection to IBKR (error 1100)"
                           : "broker disconnected from the gateway";
    const std::string msg = "WATCHDOG " + std::string(what) + " for " +
                            std::to_string(static_cast<int>(down_s)) +
                            "s during a live session - check IB Gateway";
    alerts_.notify(AlertNotifier::Critical, msg);
    route("alert: " + msg);   // console/ /logs visibility; route() does not re-scan
}

// Orphaned-position watchdog.
//
// An ADOPTED position (hot restart, or a lineup swap re-homing a symbol) leaves
// its strategy paused until the position goes flat, on the assumption that the
// adopted broker-side stop/TP is what will close it. When no protective order
// came across with it, that assumption is silently false: nothing closes it and
// nothing is watching. SNXX sat like that from 10:00 on 2026-08-06 and lost
// $846 by 15:05. The 0.10.0 engine backstop now flattens it at 15:57, but that
// is hours of unmanaged exposure — this pages as soon as it is detectable.
//
// Deliberately narrower than "unprotected": Bollinger and RSI-2 hold their OWN
// positions without a stop by design (they exit on a time stop), so a blanket
// naked-position alert would cry wolf every session. adopted && no-stop is the
// state that is always wrong.
void App::pump_orphan_watchdog() {
    static constexpr double kOrphanAlertSec = 120.0;    // let a restart settle
    static constexpr double kOrphanReAlertSec = 900.0;  // re-page every 15 min
    if (!engine_.live_running()) {
        orphan_wd_.reset();
        return;
    }
    const LiveSnapshot s = engine_.live_snapshot();
    auto has_working_stop = [&](uint32_t sid) {
        for (const OrderRecord& o : s.orders)
            if (o.symbol_id == sid && o.status == OrderStatus::Working &&
                o.type == static_cast<uint8_t>(OrdType::Stop))
                return true;
        return false;
    };
    std::string orphans;
    for (size_t i = 0; i < s.symbols.size(); ++i) {
        const SymbolState& ss = s.symbols[i];
        if (!ss.adopted || ss.position.qty == 0.0) continue;
        if (has_working_stop(static_cast<uint32_t>(i + 1))) continue;
        if (!orphans.empty()) orphans += ", ";
        orphans += ss.symbol + " " +
                   std::to_string(static_cast<long long>(ss.position.qty));
    }
    switch (orphan_wd_.update(!orphans.empty(), mono_s(), kOrphanAlertSec,
                              kOrphanReAlertSec)) {
    case WatchdogTimer::Action::None:
        return;
    case WatchdogTimer::Action::Recovered:
        alerts_.notify(AlertNotifier::Warning, "positions: no orphaned positions left");
        return;
    case WatchdogTimer::Action::Page:
        break;
    }
    const std::string msg =
        "WATCHDOG adopted position(s) with NO protective stop and a paused "
        "strategy - nothing will close them: " + orphans;
    alerts_.notify(AlertNotifier::Critical, msg);
    route("alert: " + msg);
}

// Book-divergence auditor.
//
// WHAT IT IS FOR. On 2026-08-13 client 22 adopted a take-profit that client 20
// had placed. IB routes an order's fills to the client that placed it, so when
// the limit filled the app was never told: it carried a PHANTOM 411-share
// position for 4 h 03 m, published a fictional +$528 unrealized, and reported
// the symbol "protected". Every existing signal read healthy, because there is
// no callback for a callback that does not arrive. The client-id fix removes
// that cause; this removes the blindness, which is the more durable problem — a
// dropped message, an order placed by hand in the Gateway, a future refactor all
// re-open it, and a book derived from fills can never audit itself.
//
// CADENCE: 60 s (net::kBookAuditIntervalMs). The requirement is minutes, not
// hours. The cost is provably negligible — reqPositions is an account-level
// subscription, two messages plus one per position, against IB's 50 msg/s limit,
// and it is not under the historical-data pacing rules the tournament fights
// with (net/hist_pacing.h), so it cannot starve the optimizer's bar fetches.
//
// THE IN-FLIGHT-FILL RACE, handled twice because there are two of them:
//   1. A fill that lands WHILE the broker's snapshot is in flight. The app's
//      quantities are recorded when the request goes out and compared against
//      the same quantities when the answer lands; any symbol that moved in
//      between is dropped from the round (compare_books' `settling`). Comparing
//      two books taken at different instants proves nothing.
//   2. A fill the broker has already executed and reported, that the engine has
//      not drained yet — the broker reads 0 while the book still reads 411, i.e.
//      identical to the bug. Case 1 cannot catch it, because the app side has
//      not moved yet. Time separates them instead: the engine drains its event
//      ring every frame, so a real in-flight fill cannot still be outstanding
//      60 s later, while a callback that is never coming can. Hence
//      kBookAuditConfirmRounds = 2, and the streak requires the SAME pair of
//      quantities both times — a book in motion resets it.
//
// WHY IT DOES NOT HALT THE SYMBOL. Tempting, and wrong, because the two
// directions of a divergence call for opposite actions and the detector cannot
// tell which it is looking at:
//   - app-only (the 2026-08-13 phantom): the app thinks it holds something the
//     account does not. A strategy deciding against that WOULD be dangerous —
//     "exiting" it opens a naked short — so halting looks right here.
//   - broker-only: the account holds a position the app has lost track of.
//     Halting that symbol's strategy strands a REAL position with nothing
//     managing it and no stop — precisely the orphan that cost $846 on
//     2026-08-06 and that pump_orphan_watchdog exists to page about.
// A divergence means one of the two sides is lying, and if we knew which we
// would not need the detector. Halting is therefore right in one direction,
// actively harmful in the other, and chosen blind. What is NOT symmetric is the
// backstop: the stranded-real-position case is already covered (the engine's
// 15:57 EOD flatten, the orphan watchdog, the operator's kill switch), while
// nothing anywhere would notice that this code had halted a live symbol on a
// metric that has never run in anger. So it pages, loudly, with both quantities
// named, and leaves the decision to the human — the same rule
// pump_history_watchdog settled on for the same reason.
void App::pump_book_audit() {
    // First repeat after 15 min, then x3 up to hourly. A divergence is real and
    // must never go silent, but it is also STABLE by construction — the detector
    // only opens one after two consecutive audits agree it is there — so a
    // repeat carries nothing the first page did not, and the text already quotes
    // how long it has run. On 2026-08-14 the flat 15-minute cadence paged ~32
    // times about one unchanged NVDA divergence and drowned the channel.
    static constexpr double kDivergenceReAlertSec = 900.0;      // first repeat: 15 min
    static constexpr double kDivergenceBackoffMult = 3.0;
    static constexpr double kDivergenceReAlertCapSec = 3600.0;  // never quieter than hourly
    const auto stand_down = [this] {
        book_audit_.stand_down();
        book_div_wd_.reset();
        book_blind_wd_.reset();
        book_audit_sent_.clear();
        book_div_.clear();
    };
    // Armed only when there is a session, a broker that can answer, and a book
    // worth comparing. On the IBKR web route there is no position-audit path, so
    // the auditor stands down rather than reporting a blindness that is a
    // property of the route.
    //
    // The two cheap tests come FIRST, ahead of live_snapshot(). The snapshot
    // deep-copies the session's whole order vector (up to 200 OrderRecords) and
    // every symbol's state under snap_mu_ — the same mutex the busy-spinning
    // live engine thread takes to publish — and this runs on the ~100 Hz tick,
    // so taking it before the guard meant ~100 full copies a second to read one
    // bool, plus copies on every tick with no session at all.
    // pump_orphan_watchdog orders it this way for the same reason, and
    // trade.cpp:195 records the regression that taught it.
    if (!engine_.live_running() || tws_ == nullptr) {
        stand_down();
        return;
    }
    const LiveSnapshot snap = engine_.live_snapshot();
    // `reconciled` matters just as much: before it, the engine's book is empty
    // by construction and every adopted position would read as a broker-only
    // divergence. It is not an open-ended wait — the engine flips it on
    // ReconcileEnd or a 10 s failsafe, whichever comes first.
    if (!snap.reconciled) {
        stand_down();
        return;
    }
    const int64_t now_ms = steady_ms();
    const double now_s = mono_s();
    // Arm on the first eligible frame rather than on the first ANSWER, so the
    // blind clock has an origin and /diag reads "starting" instead of "off"
    // while the very first request is outstanding. An auditor that reported
    // "off" during a live session would be the same class of lie as
    // data.connected reading true against a login modal.
    book_audit_.arm(now_ms);
    // The app's book: every session symbol, including the flat ones. A flat
    // entry is a claim ("we hold nothing here") and the broker can contradict it.
    std::vector<net::BookPos> app_book;
    app_book.reserve(snap.symbols.size());
    for (const SymbolState& ss : snap.symbols)
        app_book.push_back({ss.symbol, ss.position.qty});

    // Drain any completed audit first, so a request issued this frame is never
    // matched against an answer that predates it.
    std::vector<TwsBroker::BrokerPosition> broker;
    if (tws_->take_position_audit(broker)) {
        std::vector<net::BookPos> broker_book;
        broker_book.reserve(broker.size());
        for (const auto& p : broker) broker_book.push_back({p.symbol, p.qty});
        // Race half 1: symbols whose app-side quantity moved while the answer
        // was in flight.
        std::vector<std::string> settling;
        for (const net::BookPos& sent : book_audit_sent_)
            for (const net::BookPos& nowp : app_book)
                if (nowp.symbol == sent.symbol &&
                    std::fabs(nowp.qty - sent.qty) > net::kQtyEpsilon)
                    settling.push_back(nowp.symbol);
        // A symbol that has appeared or vanished from the book since the request
        // (a lineup swap mid-audit) is equally not comparable.
        for (const net::BookPos& nowp : app_book) {
            bool sent_it = false;
            for (const net::BookPos& s : book_audit_sent_)
                if (s.symbol == nowp.symbol) { sent_it = true; break; }
            if (!sent_it) settling.push_back(nowp.symbol);
        }
        // ...and the mirror: a symbol that was in the book when the request went
        // out and is gone now. It can no longer be compared from the app side at
        // all, and leaving it in would let the broker's row for it read as a
        // broker-only divergence purely because the lineup moved underneath.
        for (const net::BookPos& s : book_audit_sent_) {
            bool still_here = false;
            for (const net::BookPos& nowp : app_book)
                if (nowp.symbol == s.symbol) { still_here = true; break; }
            if (!still_here) settling.push_back(s.symbol);
        }
        book_audit_.observe(app_book, broker_book, settling, now_ms);
        book_audit_sent_.clear();
    }
    // Degrade to Blind on its own if answers stop. This is what stops the metric
    // reading "ok" while the thing that produces "ok" is dead — the failure that
    // pinned oldest_history_age_ms at 0 through a five-hour outage.
    const net::AuditState state = book_audit_.tick(now_ms);

    if (now_s >= book_audit_next_s_) {
        book_audit_next_s_ =
            now_s + static_cast<double>(net::kBookAuditIntervalMs) / 1000.0;
        book_audit_sent_ = app_book;   // the "before" side of race half 1
        tws_->request_position_audit();
    }

    // ---- alerts -------------------------------------------------------------
    // LATCHED, not level-triggered on Diverged: Suspect and Blind are both exits
    // from Diverged that are not recoveries, and driving the timer straight off
    // the state announced "the app's book and the broker's agree again" on
    // divergences that had never once agreed. See net::DivergenceLatch for both
    // routes. The confirmation streak IS the grace period (two audits, ~60 s
    // apart), so the timer's alert_after stays 0: a divergence that reaches
    // Diverged has already outlasted everything a transient can.
    book_div_.update(state, book_audit_.confirmed(), book_audit_.confirmed_span_ms());
    switch (book_div_wd_.update(book_div_.open(), now_s, 0.0, kDivergenceReAlertSec,
                                kDivergenceBackoffMult, kDivergenceReAlertCapSec)) {
    case WatchdogTimer::Action::Page: {
        const std::string msg =
            net::book_divergence_alert(book_div_.divergences(), book_div_.span_ms());
        alerts_.notify(AlertNotifier::Critical, msg);
        route("alert: " + msg);
        break;
    }
    case WatchdogTimer::Action::Recovered:
        // Reachable only from Ok now — an audit that came back and agreed.
        alerts_.notify(AlertNotifier::Warning,
                       "positions: the app's book and the broker's agree again");
        break;
    case WatchdogTimer::Action::None:
        break;
    }
    // The auditor's own health, paged separately: "we cannot check" is not
    // "nothing is wrong", and conflating them is how this project has been
    // blinded before. Suppressed while the order socket is down — that outage
    // has its own page (pump_broker_watchdog) and an auditor with no socket is
    // not itself broken.
    const bool socket_up = tws_->ready();
    switch (book_blind_wd_.update(socket_up && state == net::AuditState::Blind,
                                  now_s, 0.0, kDivergenceReAlertSec,
                                  kDivergenceBackoffMult, kDivergenceReAlertCapSec)) {
    case WatchdogTimer::Action::Page: {
        const std::string msg =
            net::book_audit_blind_alert(book_audit_.unanswered_ms(now_ms));
        alerts_.notify(AlertNotifier::Critical, msg);
        route("alert: " + msg);
        break;
    }
    case WatchdogTimer::Action::Recovered:
        // Only an ANSWER clears this one. The bad condition is
        // `socket_up && Blind`, so it also goes false when the order SOCKET
        // DROPS — which is the exact inverse of recovery — and the timer would
        // then announce "the broker is answering position requests again" at the
        // moment the broker stopped being reachable at all, with /diag still
        // reading "unknown: no broker answer for Ns". Still Blind means still
        // blind: the socket outage has its own page (pump_broker_watchdog), and
        // when it comes back still-blind this timer pages again on the next tick
        // because its alert_after is 0.
        if (state != net::AuditState::Blind)
            alerts_.notify(AlertNotifier::Warning,
                           "positions: the broker is answering position requests again");
        break;
    case WatchdogTimer::Action::None:
        break;
    }
}

// The series the autopilot re-fetches every cycle — the one that is supposed to
// stay fresh, and therefore the one /diag, /metrics and the watchdog measure.
std::string App::traded_bar_interval() const {
    return sweep_interval_str(sweep_panel_.settings().interval_idx);
}

// History-staleness watchdog.
//
// On 2026-08-07 the data client started failing every historical-bar request on
// a 30-minute cycle from ~10:31. SOXS/AAOX/SNDQ candles went 4.5-5.2 hours old
// while their strategies traded on, and nothing anywhere said so: data.connected
// was true (half-open socket), oldest_history_age_ms was flat (dead requests are
// cancelled at 20s and leave the pending set), and broker_upstream only watches
// the ORDERS client. pump_broker_watchdog covers the order path the same way
// this covers the data path — measured from the last SUCCESS, because every
// request-side signal is structurally blind to this failure.
//
// DIAGNOSTIC ONLY. It pages; it never halts, cancels or flattens. The right
// response is a human looking at the gateway, and an automatic reaction here
// would be a new way to stop trading on a metric that has never run in anger.
//
// GATED ON REGULAR TRADING HOURS since 0.21.0. On 2026-08-11 a session was left
// running past the 15:55 auto-stop and this paged Critical at 18:40 and 19:10 —
// "strategies are trading on stale candles" — with the market shut since 16:00
// and the book flat, and it would have gone on every 30 minutes until morning.
// The page's own claim is what makes it false rather than merely noisy: every
// order the engine emits carries outside_rth = 0, so after the close a strategy
// acting on a stale candle cannot fill anything. There is no money on the
// outcome, and a Critical page with an imperative diagnosis and no money on it
// trains the operator to swipe the next one away — including 2026-08-07's,
// which this exists for.
//
// The price of the gate is bounded and paid once, which is the test it had to
// pass before being allowed anywhere near the 2026-08-07 detection:
//   - it can only delay a session's FIRST page, by at most the fixed 45-minute
//     arming settle-in and only when the last delivery predates the open.
//     Replay 2026-08-07 (last SOXS refresh 09:17, 90-minute grace, session live
//     from 09:25) and the page still lands at 10:48, unmoved: the market had
//     been open 78 minutes by then. A stall that starts after the open pays
//     nothing at all.
//   - overnight it defers, it does not discard. 0.21.0 claimed here that no
//     evidence was being given up "because none was being kept", and that is
//     simply not true of the configuration that produced the incident: the
//     hist_fresh_.clear() it cited runs only on the !live_running() branch, and
//     a session left running never reaches it. pump_autopilot keeps cycling all
//     night too, so a data path that dies at 17:00 is genuinely visible at
//     03:00. What IS true is the narrower claim: nothing can FILL — every order
//     the engine emits carries outside_rth = 0 — so the evidence keeps until
//     45 minutes after the open, and it is still there when the gate lifts
//     because staleness is measured from the last delivery, not from the open.
//     The trade is one page moved to ~10:15 against roughly forty a night that
//     say "strategies are trading on stale candles" while no strategy can
//     trade at all.
// See tt::rth_open_elapsed_ms (market_calendar.h) for the arithmetic, including
// the 1pm early closes, and net::kHistArmSettleMs for why the settle-in is a
// fixed window rather than each symbol's own grace.
// A live session must not outlive its trading day. See App::pump_session_guard
// in app.h for the 2026-08-14 incident and tt::session_should_stop for the
// clock arithmetic (including the three 1pm early closes, which a hardcoded
// 16:15 would get wrong by three hours).
//
// It is deliberately NOT conditional on trade_sched_on, on the lineup, or on how
// the session was started. The whole defect was that the only clock-driven
// terminator in the app could be — and was — switched off, while the thing that
// STARTS unattended sessions could not.
void App::pump_session_guard() {
    if (!engine_.live_running()) return;
    if (swap_stage_ != SwapStage::None) return;   // a swap is mid-stop already
    std::time_t now_tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now_tt);
    const int sod_now = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const bool stop_now = session_should_stop(tm, session_guard_day_,
                                              session_guard_prev_sod_);
    session_guard_prev_sod_ = sod_now;   // observed, whether or not we act
    if (!stop_now) return;
    session_guard_day_ = tm.tm_yday;   // once per day, whatever happens below
    const double end_h = session_end_h(tm);
    const int late_min =
        end_h > 0.0
            ? (tm.tm_hour * 60 + tm.tm_min) - static_cast<int>(end_h * 60.0 + 0.5)
            : 0;
    // WARNING, not Critical. Reaching this line means the backstop worked; the
    // Critical case is the one nobody hears about. It still has to be said,
    // because it means the scheduled stop did not fire — the operator has a
    // configuration to fix, and on 2026-08-14 nothing anywhere announced that.
    alerts_.notify(AlertNotifier::Warning,
                   "WATCHDOG session guard: a live session was still running " +
                       std::to_string(late_min) +
                       " min after the close - stopping it. The Trade panel's "
                       "scheduled stop did not fire (check trade_sched_on / "
                       "trade_sched_stop); nothing else on any clock ends a session.");
    route_operator("session guard: stopping a live session " +
                   std::to_string(late_min) + " min past the close");
    // Through safe_stop_live, exactly like the scheduled stop, and for the same
    // reason: Engine::stop_live leaves the TWS adapter connected and holding
    // kTwsOrdersClientId until the next morning's start dials the same id.
    //
    // KEEP POSITIONS. The default safe_stop_live() runs the kill switch, which
    // is cancel_all() + flatten() — and this fires at 16:15, with the exchange
    // already shut. That sequence CANCELS the resting protective stop and then
    // sends a market flatten that will not fill until the next open, so a
    // position that survived 15:57 (the 2026-08-06 orphan is the documented
    // case: the backstop's flatten did not fill, $846) would be left with
    // nothing protecting it overnight. Strictly worse than the state the guard
    // exists to correct.
    //
    // Keeping them leaves the position AND its resting broker orders alive for
    // the next session's reconcile-and-adopt, which is the same mechanism the
    // lineup swap already depends on. The point of this guard is to end an
    // unattended SESSION, not to liquidate into a closed market.
    safe_stop_live(true);
}

void App::pump_history_watchdog() {
    static constexpr double kHistStaleReAlertSec = 1800.0;   // re-page every 30 min
    if (!engine_.live_running()) {
        // Only a delivery that lands DURING the session counts. Freshness was
        // process-lifetime, and stale() ages an answered symbol absolutely while
        // the settle-in window covers only never-answered ones — so yesterday's
        // 15:32 delivery reads as ~17 hours of staleness on the first frame of
        // the 09:25 auto-start, and the warmup fetch does not clear it either
        // (start_live_session skips that whenever seed_bars finds something, and
        // series_ still holds the previous session's bars in memory). That is
        // two Critical pages every morning telling the operator to go and check
        // a perfectly healthy gateway. Clearing on every idle frame rather than
        // on the stop edge also covers a fetch made while nothing was trading —
        // a chart pull or the daily lineup's 1d pass — which would otherwise
        // hand the next session an age it never earned.
        hist_fresh_.clear();
        hist_live_since_s_ = 0.0;
        hist_stale_last_alert_s_ = 0.0;
        return;
    }
    const double now = mono_s();
    if (hist_live_since_s_ == 0.0) hist_live_since_s_ = now;
    if (ap_.syms.empty()) return;

    // Watch only what something is actually supposed to refresh. After the
    // start-of-session warmup fetch, the autopilot's optimize cycle is the only
    // thing that re-requests a live symbol's bars (the tick stream feeds the
    // engine directly) — and pump_autopilot starts a cycle on a TIMER only for
    // trigger 0 ("Timer") or 2 ("Both"). A symbol armed on "Drawdown" alone is
    // therefore exactly as stale-by-design as an unarmed one and would page
    // every session; the Trade panel does not even show its interval_min, so
    // that field is not a cadence to size a grace off either. Each watched
    // symbol carries its own (see net::WatchedSymbol).
    std::vector<net::WatchedSymbol> watched;
    watched.reserve(ap_.syms.size());
    for (const Autopilot::Sym& S : ap_.syms) {
        if (S.mode <= 0) continue;
        if (S.trigger != 0 && S.trigger != 2) continue;   // no timer, no cadence
        watched.push_back({S.symbol, S.interval_min});
    }
    if (watched.empty()) return;

    // How long the watchdog has been ENTITLED to expect refreshes: time since
    // the LATER of this session's start and today's 09:30 open. Zero outside
    // regular hours, which is the gate — nothing can be late while the market
    // is not running. Wall clock on purpose: the open is a wall-clock
    // event, and unlike the staleness ages themselves (steady, so an NTP jump
    // around a gateway restart cannot fake hours of it) this figure is bounded
    // by a single trading day and cannot accumulate error.
    const std::time_t wall = std::time(nullptr);
    std::tm ltm{};
    localtime_s(&ltm, &wall);
    const int64_t open_ms = rth_open_elapsed_ms(ltm);
    const int64_t session_ms =
        static_cast<int64_t>((now - hist_live_since_s_) * 1000.0);
    const int64_t armed_ms = net::hist_armed_ms(session_ms, open_ms);

    const int64_t now_steady = steady_ms();
    // BOTH clocks. The delivery age must be steady (an NTP step around a gateway
    // restart would otherwise read as hours of fake staleness), and the bar age
    // must be wall — a bar's timestamp comes from the exchange and can only be
    // compared against wall time. See net/hist_freshness.h.
    const int64_t now_wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    const auto stale = hist_fresh_.stale(watched, traded_bar_interval(), now_steady,
                                         now_wall, session_ms, armed_ms);

    if (stale.empty()) {
        if (hist_stale_last_alert_s_ != 0.0) {
            // "Refreshing again" is a claim about the data path, and at 16:00 it
            // would be a false one — bars stopped because the market did. Clear
            // the episode either way (a stall that is still there tomorrow gets
            // a fresh page once the open has settled), but only say the cheerful
            // thing when it is true.
            if (open_ms > 0)
                alerts_.notify(AlertNotifier::Warning,
                               "history: bars are refreshing again");
            else
                route("history: staleness watchdog stood down - the regular "
                      "session is closed, so nothing is overdue until the open");
            hist_stale_last_alert_s_ = 0.0;
        }
        return;
    }
    if (hist_stale_last_alert_s_ != 0.0 &&
        now - hist_stale_last_alert_s_ < kHistStaleReAlertSec)
        return;
    hist_stale_last_alert_s_ = now;
    std::string detail;
    for (const net::StaleBars& sb : stale) {
        if (!detail.empty()) detail += ", ";
        // "frozen" names WHICH age this is. A symbol whose deliveries are on
        // time and whose newest bar is 300 minutes old is a different fault
        // from one nothing has answered for 300 minutes, and the operator
        // cannot tell them apart from a bare number.
        detail += sb.symbol + " " +
                  (sb.ever ? std::to_string(sb.age_ms / 60'000) + "m" +
                                 (sb.frozen ? " frozen" : "")
                           : std::string("never"));
    }
    // The wording is net::hist_stall_alert's, not this function's: the old text
    // ended "check IB Gateway" on evidence that argues the opposite (the same
    // socket and farms serve the healthy symbols in the very cycles the others
    // die), and a page that sends the operator to restart a healthy gateway is
    // one worth being able to test. See net/hist_freshness.h.
    //
    // The page names the symbols that are DEMONSTRABLY still being served —
    // symbols with a recent delivery — not the ones that merely have not
    // crossed their own grace yet. On a lineup of mixed autopilot cadences the
    // second set includes symbols that have delivered nothing all session.
    const std::string msg = net::hist_stall_alert(
        detail, stale,
        hist_fresh_.refreshing(watched, traded_bar_interval(), now_steady),
        data_.connected());
    alerts_.notify(AlertNotifier::Critical, msg);
    route("alert: " + msg);
}

// Pre-open gateway AUTHENTICATION check — a WINDOW, 08:45-09:15 local, on days
// the market actually opens.
//
// 2026-08-09: the 00:55 cold restart's automated re-login failed and the gateway
// sat on "UNRECOGNIZED USERNAME OR PASSWORD" for 13 hours. Nothing paged,
// because every check we had is gated on a live session and there is no live
// session overnight: pump_broker_watchdog returns on !live_running(), /diag
// broker_connected is false all night by design, and the external keepalive only
// acts on live && !broker_connected. The process was up and port 4002 was
// listening the whole time, so the two things a human (and an agent) actually
// looked at both said "healthy".
//
// This is the one check that runs with NO session — that is the entire point —
// so it must touch nothing that assumes one exists. It reads gateway login state
// (net/gateway_auth.h, proven by the data farms) ahead of the 09:25 scheduled
// auto-start, the last moment a human can still fix it by hand before the open.
//
// A WINDOW WITH A SETTLE TIME, not a clock edge. 0.14.0 fired on the frame that
// first observed the clock crossing 08:45, which broke three ways:
//   - an app started at 08:45:01 (a deploy, a crash restart, the operator's
//     routine) had already "missed" the crossing and skipped the day silently —
//     losing the check in the exact window a restart is most likely;
//   - an app started at 08:44:40 fired 20 seconds in, before TwsData could
//     connect, and cold-restarted a gateway that was only ever fine;
//   - the crossing was sampled only on drawn frames, so minimizing the window
//     both lost it and left it armed to fire at an arbitrary later hour.
// Level-triggered inside the window with a day stamp instead — the same shape
// pump_lineup_schedule uses "so a late launch still catches it" — plus a
// continuous-failure settle time, because a momentary "not authed" is what a
// data-socket reconnect looks like from here: the reconnect loop calls
// drop_connection() on every pass while disconnected, and that wipes all auth
// proof by design (session_lost).
//
// DIAGNOSTIC first, and at most one relaunch: IBKR LOCKS ACCOUNTS ON REPEATED
// FAILED LOGINS, so this may never loop. See the guards on the relaunch below.
void App::pump_preopen_gateway_check() {
    static constexpr double kWindowStartH = 8.75;   // 08:45 local
    static constexpr double kWindowEndH = 9.25;     // 09:15, still ahead of 09:25
    static constexpr double kSettleSec = 150.0;     // continuously not-authed
    static constexpr double kMinUptimeSec = 180.0;  // > the 90s gateway_starting_until_
    static constexpr double kVerdictSec = 480.0;    // kill + JVM + login + disclaimer + farms
    // TWS route only: gateway_authed() is proven by the TWS data farms and the
    // ibkr_web source has no evidence to offer, so it answers false forever —
    // running this there would page every morning and relaunch nothing.
    if (!use_tws_data_) return;

    const double now = mono_s();

    // Outcome of a relaunch. POLLED, not sampled once: 0.14.0 looked exactly
    // once at +240s and pages "the relaunch did NOT restore the login - LOG IN
    // BY HAND" if that instant missed, with nothing to retract it afterwards.
    // The real budget is longer and highly variable — Start-IbGateway.ps1 alone
    // allows 120s for the API port, then IBC has to clear the paper-trading
    // disclaimer (the 10141 backoff in tws_data), then TwsData reconnects on a
    // 3s cycle, and only then do the farms report. So: page recovery the moment
    // it recovers, and page failure only once the whole budget is gone.
    if (preopen_verdict_until_ != 0.0) {
        if (preopen_authed()) {
            preopen_verdict_until_ = 0.0;
            const std::string msg =
                "PRE-OPEN gateway relaunch worked - IB Gateway is logged in to "
                "IBKR again, the 09:25 auto-start should be fine";
            alerts_.notify(AlertNotifier::Warning, msg);   // recovery, as the other watchdogs do
            route("alert: " + msg);
        } else if (now >= preopen_verdict_until_) {
            preopen_verdict_until_ = 0.0;
            const std::string msg =
                "WATCHDOG PRE-OPEN the gateway relaunch did NOT restore the IBKR "
                "login - LOG IN BY HAND (RDP) before 09:25 or the auto-started "
                "session will trade blind. No further automatic attempt will be "
                "made today: IBKR locks accounts on repeated failed logins";
            alerts_.notify(AlertNotifier::Critical, msg);
            route("alert: " + msg);
        }
    }

    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    const int64_t day = static_cast<int64_t>(tm.tm_year) * 400 + tm.tm_yday;
    const double hod = tm.tm_hour + tm.tm_min / 60.0;
    // is_us_trading_day, not just a weekday: the recovery below spends a login
    // attempt, and spending one on Thanksgiving or Good Friday buys nothing and
    // aims it straight into an IBKR holiday maintenance window — the same
    // window that produces the misleading credential error. See market_calendar.h.
    const bool armed = hod >= kWindowStartH && hod < kWindowEndH &&
                       is_us_trading_day(tm) && day != preopen_day_;
    if (!armed || now < kMinUptimeSec) {
        // The streak means nothing outside the window, and a just-started app
        // has not had time to connect to anything yet — counting either as
        // evidence is how a healthy gateway gets killed.
        preopen_bad_since_ = 0.0;
        return;
    }
    if (preopen_authed()) {   // healthy: silent, by design, and re-armed
        preopen_bad_since_ = 0.0;
        return;
    }
    if (preopen_bad_since_ == 0.0) preopen_bad_since_ = now;
    if (now - preopen_bad_since_ < kSettleSec) return;
    preopen_day_ = day;

    const int64_t age = data_.gateway_auth_age_ms();
    const std::string since =
        age < 0 ? std::string("nothing at all on this gateway session")
                : std::string("last ") + std::to_string(age / 60'000) + " min ago";
    // Two genuinely different faults, and the operator has ~40 minutes to act on
    // whichever it is, so do not blur them. Socket down = we cannot even reach
    // the gateway (the keepalive's port-down path owns that recovery, and it
    // must not be raced). Socket up = the 2026-08-09 shape exactly.
    const bool socket_up = data_.connected();
    // THIRD cause, and the app can prove this one outright: the gateway is up
    // and answering, and it is refusing US (IB error 326). Checked first because
    // the "cannot reach the gateway" sentence below is flatly false in that
    // case, and it would send the operator to hand-restart a healthy gateway —
    // minutes after the API CLIENT ID IN USE line paged them with the truth.
    // Two contradictory Critical pages is worse than one.
    const bool id_conflict = data_.client_id_conflict();
    std::string msg =
        id_conflict
            ? "WATCHDOG PRE-OPEN IB Gateway is UP and healthy but is REFUSING our "
              "market-data client id - something else is connected with it (IB "
              "error 326; see the API CLIENT ID IN USE line for which id and what "
              "to close). We are still retrying. "
            : socket_up
            ? "WATCHDOG PRE-OPEN IB Gateway is UP but NOT logged in to IBKR - it "
              "has told the data client " + since + ". "
            : "WATCHDOG PRE-OPEN cannot reach IB Gateway on the API port at all "
              "(no data session; it has said " + since + "). ";
    // The consequence differs, and saying the wrong one wastes the operator's
    // ~40 minutes. A refused DATA client costs candles; it does not by itself
    // stop orders, because the broker takes a rotating id of its own (20-39).
    msg += id_conflict
               ? "The 09:25 auto-start will bring up a session with no candles - "
                 "no warmup, no lineup, no charts - even if its order path "
                 "connects on its own client id. "
               : "The 09:25 auto-start will bring up a session that cannot reach "
                 "the market. The CREDENTIALS ARE PROBABLY FINE: IBKR answers "
                 "'unrecognized username or password' during its maintenance "
                 "window (2026-08-09 lost 13 hours to exactly that). ";

    // ---- at most one recovery attempt, ever, per day ----
    //
    // IBKR LOCKS ACCOUNTS ON REPEATED FAILED LOGINS. A locked account is a far
    // worse outage than the one being recovered from — it cannot be fixed from
    // here at all. Three separate things bound this, because the day stamp alone
    // demonstrably cannot:
    //   1. this stamp, set BEFORE the launch, so a crash between the two costs
    //      nothing and no future edit to the trigger can turn one attempt into
    //      many;
    //   2. PAPER ONLY (below), because -Restart is a COLD relaunch;
    //   3. Start-IbGateway.ps1's own cross-process launch mutex + login-attempt
    //      governor, which is the only guard that also binds Watch-IbGateway.ps1
    //      — this relaunch takes the API port down, which is exactly what arms
    //      the keepalive's own port-down relaunch, and the keepalive cannot see
    //      an in-process day stamp. That collision is what would actually have
    //      locked the account.
    // !id_conflict is belt-and-braces (socket_up is already false when we are
    // being refused), but it is stated because the consequence is severe: a
    // cold relaunch aimed at a 326 kills a gateway that is working, drops
    // whatever legitimately holds the id, and spends one of the day's very few
    // login attempts on a fault the retry loop is already handling.
    bool relaunch = socket_up && !id_conflict && preopen_relaunch_day_ != day;
    // A LIVE cold re-login may demand a TOTP that IBC cannot type, and would
    // then wedge on the 2FA dialog with no login at all — strictly worse than
    // the state being recovered from, and unfixable without RDP. This is the
    // same reason Start-IbGateway.ps1 writes AutoRestartTime (soft) rather than
    // ColdRestartTime for a live account. Live gets the page and nothing else.
    const bool paper = active_account_is_paper();
    if (relaunch && !paper) {
        relaunch = false;
        msg += "NOT relaunching: this is a LIVE account and a cold re-login can "
               "stop on a 2FA prompt IBC cannot answer. LOG IN BY HAND (RDP)";
    } else if (!relaunch) {
        // "Log in by hand" is the wrong instruction for a client-id collision:
        // the gateway IS logged in, and a second login would achieve nothing.
        msg += id_conflict
                   ? "Not relaunching: the gateway is fine and the login is not "
                     "the problem. CLOSE WHATEVER HOLDS THE CLIENT ID"
                   : "Not relaunching automatically - LOG IN BY HAND (RDP)";
    }

#ifdef _WIN32
    const std::string args =
        relaunch ? ps_args("Start-IbGateway.ps1", true, "-Restart") : std::string();
    if (relaunch && args.empty()) {
        relaunch = false;
        msg += "Cannot relaunch: scripts\\Start-IbGateway.ps1 is missing. LOG IN "
               "BY HAND (RDP)";
    } else if (relaunch) {
        msg += "Trying ONE relaunch now";
    }
#else
    if (relaunch) {
        relaunch = false;
        msg += "Automatic relaunch is Windows-only. LOG IN BY HAND (RDP)";
    }
#endif

    alerts_.notify(AlertNotifier::Critical, msg);
    route("alert: " + msg);
    if (!relaunch) return;

#ifdef _WIN32
    // -Restart, not a bare start: Start-IbGateway.ps1 exits immediately when the
    // API port is already up (it is idempotent by design, and the app calls it
    // on every launch), which is precisely the state we are in. -Restart kills
    // the gateway and relaunches it with ONE fresh login — and the script's
    // governor will refuse even that if logins have already been attempted too
    // often, leaving the gateway untouched.
    preopen_relaunch_day_ = day;
    run_hidden(args);
    route("gateway: PRE-OPEN relaunching IB Gateway (one attempt; verdict within " +
          std::to_string(static_cast<int>(kVerdictSec)) + "s)");
    preopen_verdict_until_ = now + kVerdictSec;
#endif
}

// "The gateway is logged in", for the pre-open check.
//
// The farms are the overnight signal, but a live ORDER path that is reaching
// IBKR proves the same thing more directly, so it counts too. That second term
// is not redundant: it is what stops a false negative here from killing a
// healthy session, since the recovery is a gateway relaunch. Overnight tws_ is
// null and it contributes nothing — which is the blindness being fixed.
bool App::preopen_authed() const {
    if (data_.gateway_authed()) return true;
    return tws_ && tws_->ready() && tws_->upstream_connected();
}

void App::refresh_ibkr_accounts() {
    const auto a = read_ibkr_accounts();
    signin_.ibkr_accounts.clear();
    signin_.ibkr_labels.clear();
    signin_.ibkr_paper.clear();
    signin_.ibkr_readonly.clear();
    for (const auto& acc : a.accounts) {
        signin_.ibkr_accounts.push_back(acc.name);
        signin_.ibkr_labels.push_back(acc.label);
        signin_.ibkr_paper.push_back(acc.paper ? 1 : 0);
        signin_.ibkr_readonly.push_back(acc.readonly ? 1 : 0);
    }
    signin_.ibkr_active = a.active;
    const int n = static_cast<int>(signin_.ibkr_accounts.size());
    if (signin_.ibkr_selected >= n) signin_.ibkr_selected = -1;
    if (signin_.ibkr_selected < 0) {   // default to the active account
        for (int i = 0; i < n; ++i)
            if (signin_.ibkr_accounts[i] == a.active) { signin_.ibkr_selected = i; break; }
        if (signin_.ibkr_selected < 0 && n > 0) signin_.ibkr_selected = 0;
    }
}

void App::draw_account_modal() {
    if (signin_.account_request_open) {
        signin_.account_request_open = false;
        signin_.account_open = true;
        ImGui::OpenPopup("Account");
    }
    // p_open shows a title-bar X (closes the popup like a normal window).
    if (!ImGui::BeginPopupModal("Account", &signin_.account_open,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Don't fade the app behind this dialog (see draw_data_modal for the why).
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = 0.0f;

    // Provider dropdown: only IBKR today, kept so more brokers can slot in.
    static constexpr const char* kBrokers[] = {"Interactive Brokers Canada"};
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Provider", &signin_.broker_provider, kBrokers, IM_ARRAYSIZE(kBrokers));
    ImGui::Separator();

    refresh_ibkr_accounts();   // pick up accounts added via "Add New"

    const bool up = data_.connected();
    const bool initializing = !up && mono_s() < gateway_starting_until_;

    // Kick off a sign-in (disconnected) or switch (connected, different
    // account) for `name`. Both run Switch-IbkrAccount, which tears the old
    // session down first. TWS route: that script only drives the CP web
    // gateway — switching means re-logging IB Gateway, done via script.
    auto do_switch = [&](const std::string& name) {
        if (use_tws_data_) {
            route("account: TWS route - switch with scripts\\Start-IbGateway.ps1 "
                     "-Stop, then Start-IbGateway.ps1 -Account \"" + name + "\"");
            return;
        }
        const std::string args = switch_args(name);
        if (args.empty()) return;
        run_hidden(args);
        gateway_starting_until_ = mono_s() + 90.0;
        route(std::string("account: ") +
                 (up ? "switching to IBKR '" : "signing in to IBKR '") + name + "'");
    };

    if (signin_.ibkr_accounts.empty()) {
        ImGui::TextDisabled("None saved yet - click \"Add New\".");
    } else {
        const float ch = ImGui::GetFontSize();
        for (int i = 0; i < static_cast<int>(signin_.ibkr_accounts.size()); ++i) {
            const std::string& name = signin_.ibkr_accounts[i];
            // Display the friendly label but ID the row by the unique key,
            // so two accounts sharing a label (paper + live) don't collide.
            const std::string row = signin_.ibkr_labels[i] + "##" + name;
            const bool active = up && name == signin_.ibkr_active;
            // A real checkmark (drawn with primitives, no font glyph) marks
            // the currently signed-in account, on the left of the row.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (active)
                ImGui::RenderCheckMark(ImGui::GetWindowDrawList(),
                                       ImVec2(p.x + 2.0f, p.y + ImGui::GetStyle().FramePadding.y),
                                       ImGui::GetColorU32(ImGuiCol_CheckMark), ch * 0.72f);
            ImGui::Dummy(ImVec2(ch + 4.0f, ch));
            ImGui::SameLine();
            // Clicking a row signs in to it (or switches to it); clicking the
            // already-signed-in one does nothing. Confirm first if a live
            // trading session is running.
            if (ImGui::Selectable(row.c_str(), active,
                                  ImGuiSelectableFlags_DontClosePopups, ImVec2(190, 0)) &&
                !active && !initializing) {
                if (engine_.live_running()) {
                    signin_.pending_account = name;
                    ImGui::OpenPopup("Confirm account change");
                } else {
                    do_switch(name);
                }
            }
            // PAPER/LIVE badge, then the read-only tag pinned to a fixed
            // column so it lines up whether the badge is "PAPER" or "LIVE".
            ImGui::SameLine();
            const float badge_x = ImGui::GetCursorPosX();
            const bool paper = signin_.ibkr_paper[i] != 0;
            ImGui::TextColored(paper ? ImVec4(0.25f, 0.85f, 0.45f, 1)
                                     : ImVec4(0.95f, 0.30f, 0.25f, 1),
                               paper ? "PAPER" : "LIVE");
            if (signin_.ibkr_readonly[i]) {
                ImGui::SameLine(badge_x + ImGui::CalcTextSize("PAPER").x +
                                ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1), "read-only");
            }
        }
    }

    // Live trading is active: flatten + stop it, then switch.
    if (ImGui::BeginPopupModal("Confirm account change", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A live trading session is running. Continuing will cancel "
                           "open orders, flatten positions, and stop the session before "
                           "changing accounts.");
        ImGui::Spacing();
        if (ImGui::Button("Stop trading & switch")) {
            safe_stop_live();
            do_switch(signin_.pending_account);
            signin_.pending_account.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            signin_.pending_account.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::Button("Add New")) {
        const std::string args = addnew_args();
        if (!args.empty()) {
            ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(),
                          nullptr, SW_SHOWNORMAL);   // visible: prompts for credentials
            route("account: add IBKR account (enter credentials in the console)");
        } else {
            route("account: Save-IbkrCred.ps1 not found");
        }
    }
    ImGui::EndPopup();
}

void App::draw_data_modal() {
    if (signin_.data_request_open) {
        signin_.data_request_open = false;
        signin_.status.store(0);
        {
            std::lock_guard lock(signin_.mu);
            signin_.detail.clear();
        }
        signin_.data_open = true;
        ImGui::OpenPopup("Data");
    }
    if (!ImGui::BeginPopupModal("Data", &signin_.data_open,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Don't fade the app behind this dialog. Reassert here — only while it's
    // open, so there's no per-frame cost otherwise — so a runtime style change
    // (e.g. the Style Editor) can't bring the fade back.
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = 0.0f;

    auto make_account = [&] {
        Account a;
        a.name = signin_.name[0] ? signin_.name : "default";
        // Provider dropdown: 0 = Polygon, 1 = Finnhub.
        a.provider = signin_.provider == 1 ? "finnhub" : "polygon";
        a.key_id = signin_.key;
        a.secret = "";
        return a;
    };

    const int status = signin_.status.load();
    if (status == 2) {   // worker verified the credentials — save and close
        if (signin_.worker.joinable()) signin_.worker.join();
        const Account a = make_account();
        accounts_.upsert(a, /*make_active=*/true);
        std::string detail;
        {
            std::lock_guard lock(signin_.mu);
            detail = signin_.detail;
        }
        route("account: signed in to " + a.provider + " as '" + a.name + "' (" +
                 detail + ")");
        signin_.status.store(0);
        signin_.secret[0] = '\0';   // no plaintext left in the UI buffer
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool busy = status == 1;
    ImGui::BeginDisabled(busy);
    static constexpr const char* kFeeds[] = {"Polygon", "Finnhub"};
    ImGui::SetNextItemWidth(280);
    ImGui::Combo("Provider", &signin_.provider, kFeeds, IM_ARRAYSIZE(kFeeds));
    ImGui::SetItemTooltip("Real-time market-data source. The IBKR gateway is also a "
                          "feed, needs no key, and is picked in the Trade panel.");
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("Name", signin_.name, sizeof signin_.name);
    ImGui::SetItemTooltip("Label shown in the Data menu (e.g. \"data\")");
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("API key", signin_.key, sizeof signin_.key,
                     ImGuiInputTextFlags_Password);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Stored encrypted (Windows DPAPI, this user only).");

    if (busy) {
        ImGui::TextDisabled("Verifying...");
    } else if (status == 3) {
        std::lock_guard lock(signin_.mu);
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "Failed: %s",
                           signin_.detail.c_str());
    }

    const bool have_input = signin_.key[0] != '\0';
    ImGui::BeginDisabled(busy || !have_input);
    if (ImGui::Button("Verify & Sign In")) {
        if (signin_.worker.joinable()) signin_.worker.join();
        signin_.status.store(1);
        const std::string key = signin_.key;
        const int prov = signin_.provider;
        signin_.worker = std::thread([this, key, prov] {
            std::string detail;
            const bool ok = prov == 1
                ? finnhub_verify_key(FinnhubFeedConfig{}.rest_url, key, detail)
                : polygon_verify_key(PolygonFeedConfig{}.rest_url, key, detail);
            {
                std::lock_guard lock(signin_.mu);
                signin_.detail = std::move(detail);
            }
            signin_.status.store(ok ? 2 : 3);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Save without verifying")) {
        const Account a = make_account();
        accounts_.upsert(a, /*make_active=*/true);
        route("account: saved " + a.provider + " '" + a.name + "' (not verified)");
        signin_.secret[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);   // let the worker finish; it writes signin_ state
    if (ImGui::Button("Cancel")) {
        signin_.secret[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    draw_account_menu();
    draw_data_menu();
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Chart", nullptr, &show_chart_);
        ImGui::MenuItem("Watchlist", nullptr, &show_watchlist_);
        ImGui::MenuItem("Backtest", nullptr, &show_backtest_);
        ImGui::MenuItem("Replay", nullptr, &show_replay_);
        ImGui::MenuItem("Optimizer", nullptr, &show_sweep_);
        ImGui::MenuItem("Strategy", nullptr, &show_strategy_);
        ImGui::MenuItem("Build Output", nullptr, &show_build_output_);
        ImGui::MenuItem("Trade", nullptr, &show_trade_);
        ImGui::MenuItem("Blotter", nullptr, &show_blotter_);
        ImGui::MenuItem("Positions", nullptr, &show_positions_);
        ImGui::MenuItem("Journal", nullptr, &show_journal_);
        ImGui::MenuItem("Log Console", nullptr, &show_log_);
        ImGui::MenuItem("Optimizer Log", nullptr, &show_opt_log_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
    if (ImGui::BeginMenu("Trade")) {
        const bool busy = lineup_active();
        const bool ok = use_tws_data_ && data_.connected() && !engine_.running() &&
                        !tourn_.active && !sweep_.running;
        if (ImGui::MenuItem(busy ? "Building lineup…" : "Build today's lineup",
                            nullptr, false, ok && !busy))
            start_daily_lineup();
        ImGui::SetItemTooltip(
            "Scan IBKR for high-volatility movers, rank by ATR%%, run the strategy "
            "tournament on the top picks, and load them into the Trade tabs.\n"
            "Requires the IBKR (TWS) data route with no live session running.");

        ImGui::Separator();
        ImGui::MenuItem("Auto-build daily", nullptr, &cfg_.lineup_enabled);
        ImGui::SetItemTooltip(
            "Run the lineup build automatically each weekday at the time below.\n"
            "It won't interrupt a running session or an in-flight build.");
        ImGui::BeginDisabled(!cfg_.lineup_enabled);
        ImGui::SetNextItemWidth(52);
        if (ImGui::InputText("build time", lineup_build_buf_, sizeof lineup_build_buf_))
            cfg_.lineup_build_time = lineup_build_buf_;
        ImGui::SetItemTooltip("Local clock, HH:MM (24h). 09:35 gives the open a few\n"
                              "minutes so the volatility ranking sees real range.");
        bool autostart = !cfg_.lineup_propose_only;
        if (ImGui::MenuItem("Auto-start the session", nullptr, &autostart))
            cfg_.lineup_propose_only = !autostart;
        ImGui::SetItemTooltip(
            "On: after the daily build, start trading the picks automatically.\n"
            "Off (default): build + log the picks only — you start the session.");
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
        if (ImGui::BeginMenu("Notifications")) {
            ImGui::TextDisabled(alerts_.has_webhook()
                                    ? "ntfy categories"
                                    : "No webhook set (config.json alert_webhook)");
            ImGui::Separator();
            struct Row {
                const char* label;
                bool* flag;
                AlertCategory cat;
                const char* tip;
            };
            const Row rows[] = {
                {"Trades", &cfg_.notify_trades, AlertCategory::Trades,
                 "Every live fill. The chattiest category by far, though a\n"
                 "partially filled order pages once per burst, not once per fill."},
                {"Orders", &cfg_.notify_orders, AlertCategory::Orders,
                 "Orders refused or rejected. A refused ENTRY is routine; a\n"
                 "refused EXIT counts as Risk, not Orders, and stays on that switch."},
                {"Risk", &cfg_.notify_risk, AlertCategory::Risk,
                 "Risk halts, the kill switch, the EOD backstop, and any position\n"
                 "left with nothing to close it.\n\n"
                 "OFF MEANS NO PAGE when a position is naked. The event still\n"
                 "happens and is still logged - you simply are not told."},
                {"Connection", &cfg_.notify_connection, AlertCategory::Connection,
                 "Gateway, broker socket, data feed and client-id collisions.\n"
                 "The nightly IBKR restart is routine and already stays quiet;\n"
                 "what reaches you here is an outage that LASTED."},
                {"Integrity", &cfg_.notify_integrity, AlertCategory::Integrity,
                 "The app and the broker disagreeing about what is held, and\n"
                 "positions this session can neither see nor close.\n\n"
                 "OFF MEANS NO PAGE when the book is wrong. On 2026-08-13 that\n"
                 "state went unnoticed for four hours."},
                {"Lineup", &cfg_.notify_lineup, AlertCategory::Lineup,
                 "The daily build - symbols excluded, a day refused outright -\n"
                 "and the session guard that ends the trading day."},
                {"System", &cfg_.notify_system, AlertCategory::System,
                 "Anything matching no category above."},
            };
            for (const Row& r : rows) {
                if (ImGui::MenuItem(r.label, nullptr, r.flag)) {
                    alerts_.set_category_enabled(static_cast<int>(r.cat), *r.flag);
                    save_config();   // persist the instant it is toggled
                }
                ImGui::SetItemTooltip("%s", r.tip);
                // The two categories that report a position nothing is
                // protecting are marked when off, so a silenced safety channel
                // is visible without opening the tooltip.
                if (alert_category_is_safety(r.cat) && !*r.flag) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(!)");
                }
            }
            ImGui::Separator();
            // The master switch, moved here from View: View is about which
            // PANELS are open, and this was the only notification control there
            // was - all or nothing, which is why it stayed on.
            bool alerts_on = !alerts_.muted();
            if (ImGui::MenuItem("All notifications", nullptr, &alerts_on))
                alerts_.set_muted(!alerts_on);
            ImGui::SetItemTooltip(
                alerts_.has_webhook()
                    ? "Master switch. Off silences the beep AND the webhook for\n"
                      "every category, including any marked (!)."
                    : "Beeps on halts/rejects/disconnects. Set \"alert_webhook\"\n"
                      "in config.json (or TT_ALERT_WEBHOOK) to page a phone, e.g.\n"
                      "an ntfy.sh topic URL.");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
#ifdef TT_DEBUG
    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("Simulate ticks", nullptr, &sim_ticks_);
        ImGui::SetItemTooltip("Feed the live session a 2 Hz random walk — "
                              "test strategies while the market is closed");
        ImGui::EndMenu();
    }
#endif
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(update_check_wait_ ? "Checking for updates…"
                                               : "Check for updates",
                            nullptr, false, !update_check_wait_))
            update_check_click_ = true;
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImPlot Demo", nullptr, &show_implot_demo_);
        ImGui::EndMenu();
    }

    // Right-aligned gateway session indicator.
    const bool up = data_.connected();
    const bool initializing = !up && mono_s() < gateway_starting_until_;
    const char* label = up ? "GATEWAY UP" : (initializing ? "INITIALIZING" : "GATEWAY DOWN");
    const ImVec4 col = up ? ImVec4(0.25f, 0.85f, 0.45f, 1.0f)
                          : (initializing ? ImVec4(0.95f, 0.80f, 0.25f, 1.0f)
                                          : ImVec4(0.95f, 0.35f, 0.25f, 1.0f));
    const float w = ImGui::CalcTextSize(label).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextColored(col, "%s", label);
    ImGui::EndMainMenuBar();
}

void App::setup_default_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.34f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Watchlist", left);
    ImGui::DockBuilderDockWindow("Chart", center);
    ImGui::DockBuilderDockWindow("Backtest", right);
    ImGui::DockBuilderDockWindow("Strategy", right);
    ImGui::DockBuilderDockWindow("Trade", right);
    ImGui::DockBuilderDockWindow("Blotter", bottom);
    ImGui::DockBuilderDockWindow("Positions", bottom);
    ImGui::DockBuilderDockWindow("Journal", bottom);
    // Log Console last so it is the bottom node's selected tab on a first run:
    // ImGui selects the newest tab added to a node, and the log is what you
    // want in front. Optimizer Log and Build Output share the node but are
    // docked ahead of it so neither takes the front on launch.
    ImGui::DockBuilderDockWindow("Build Output", bottom);
    ImGui::DockBuilderDockWindow("Optimizer Log", bottom);
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
