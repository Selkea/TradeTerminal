#include "app.h"
#include "build_info.h"   // TT_GIT_COMMIT / TT_GIT_DIRTY, stamped at build time
#include "dev_paths.h"

#include "imgui_internal.h"  // DockBuilder API (default first-run layout) + private dock node flags
#include "implot.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA (gateway login page / launch)
#endif

#include "engine/ack_latency.h"
#include "engine/version.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <utility>

namespace tt::ui {

namespace {
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
      trade_(engine_),
      blotter_(engine_),
      positions_(engine_),
      sweep_panel_(engine_),
      accounts_((data_dir() / "accounts.json").string()) {
    net::IMarketData::Callbacks cbs;
    cbs.on_log = [this](std::string line) { log_.add(std::move(line)); };
    cbs.on_tick = [this](const std::string& sym, const Quote& q) {
        quotes_.set(sym, q);
        // When the real-time feed owns the session, delayed sidecar quotes
        // still update the watchlist but must not reach the engine.
        if (engine_.live_running() && !rt_feed_active_.load(std::memory_order_relaxed))
            engine_.push_live_tick(sym, q.ts_ms, q.price, q.day_volume);
    };
    cbs.on_error = [this](uint32_t id, std::string code, std::string msg) {
        log_.add("feed error (req " + std::to_string(id) + ") " + code + ": " + msg);
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_.active = false;  // data never arrives; don't wedge the panel
    };
    cbs.on_candles = [this](net::CandleBatch&& b) {
        log_.add("candles: " + b.symbol + " " + b.interval + " x" +
                 std::to_string(b.candles.size()) + (b.cached ? " (cache)" : ""));
        start_pending_backtest(b);
        stash_pending_sweep(b);
        series_.put(b.symbol, b.interval, std::move(b.candles), b.cached);
    };

    // Session persistence (cfg_ is loaded in the init list) + file logging.
    log_.set_log_file((data_dir() / "logs" / "terminal.log").string());
    watchlist_.set_symbols(cfg_.watchlist);
    chart_.restore(cfg_.chart_symbol, cfg_.chart_interval_idx, cfg_.chart_range_idx);
    backtest_.set_cash(cfg_.backtest_cash);
    trade_.restore(cfg_.trade_cash, cfg_.trade_bar_sec, cfg_.trade_data_idx,
                   cfg_.trade_record, cfg_.trade_route);
    trade_.restore_schedule(cfg_.trade_sched_on, cfg_.trade_sched_start,
                            cfg_.trade_sched_stop);
    {
        RiskLimits r;
        r.max_order_qty = cfg_.risk_max_order_qty;
        r.max_position_qty = cfg_.risk_max_position_qty;
        r.daily_max_loss = cfg_.risk_daily_max_loss;
        r.stale_feed_sec = cfg_.risk_stale_feed_sec;
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
    }
    // Rebuild last session's strategies and their params.
    strat_mgr_.restore_state(cfg_.strategy_loaded, cfg_.strategy_params);
    const char* wh = std::getenv("TT_ALERT_WEBHOOK");
    alerts_.set_webhook(wh && *wh ? wh : cfg_.alert_webhook);
    if (alerts_.has_webhook()) log_.add("alerts: webhook configured");

    if (!journal_.open((data_dir() / "journal.db").string()))
        log_.add("journal: could not open journal.db — history disabled");

#ifdef TT_DEBUG
    sim_ticks_ = std::getenv("TT_SIM_TICKS") != nullptr;
#endif

    if (use_tws_data_) {
        tws_data_.set_endpoint("127.0.0.1", tws_api_port());
        tws_data_.start(std::move(cbs));
        log_.add("data: TWS route - charts/watchlist/backtests ride IB Gateway; "
                 "CP web gateway stays down (one brokerage session per username)");
    } else {
        gw_.start(std::move(cbs));
    }

#ifdef _WIN32
    // Tie the broker gateway to this app's lifetime. Web route: CP gateway +
    // IBeam auto-login, stopped again in the destructor. TWS route: IB Gateway
    // via IBC - started if not already up, and left running on exit so the
    // login (and any typed 2FA code) survives app restarts.
    {
        const auto ib = read_ibkr_accounts();
        if (!ib.accounts.empty() && !ib.active.empty()) {
            const std::string args =
                use_tws_data_ ? ps_args("Start-IbGateway.ps1")
                              : ps_args("Start-IbkrLogin.ps1", true, "-Daemon");
            if (!args.empty()) {
                run_hidden(args);
                gateway_starting_until_ = 90.0;   // show INITIALIZING until it connects
                log_.add(use_tws_data_
                             ? "account: starting IB Gateway (TWS route)"
                             : "account: starting IBKR gateway + auto-login (tied to app)");
            }
        }
    }
#endif

    session_start_ = std::time(nullptr);
    start_diag_server();
}

// IPC thread: if this candle batch is the one a queued backtest is waiting
// for, convert it and launch the engine. The lock is held through the engine
// start so the UI thread's lease GC can never observe "pending consumed +
// engine idle" while the leased instance is being handed to the engine.
void App::start_pending_backtest(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    if (!pending_bt_.active || pending_bt_.symbol != batch.symbol ||
        pending_bt_.interval != batch.interval)
        return;
    pending_bt_.active = false;   // GC-safe: engine start happens under the lock
    if (batch.candles.size() < 3) {
        log_.add("backtest: not enough data for " + batch.symbol);
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
    log_.add(static_cast<uint64_t>(cfg_.measured_lat_count) >= kMinLatSamples
                 ? "backtest fill latency: measured " +
                       std::to_string(bt_lat / 1'000'000) + "+/-" +
                       std::to_string(bt_jit / 1'000'000) + " ms (" +
                       std::to_string(cfg_.measured_lat_count) + " live acks)"
                 : "backtest fill latency: default 75 ms (no live acks yet)");
    if (!engine_.start_backtest(std::move(cfg), pending_bt_.strategy))
        log_.add("backtest: engine busy, try again");
}

// UI thread: lease a fresh instance of `key`, capture params, fetch data.
void App::queue_backtest(const std::string& key, const std::string& sym,
                         const std::string& ivl, const std::string& rng,
                         double cash) {
    if (!data_.connected()) {
        log_.add("backtest: feed is down, cannot fetch data");
        return;
    }
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        log_.add("backtest: strategy '" + key + "' is not loaded");
        return;
    }
    leases_.push_back({inst, key, StrategyLease::Backtest});
    {
        std::lock_guard lock(pending_bt_mu_);
        pending_bt_ = {true, sym, ivl, strat_mgr_.param_values(key), cash, inst};
    }
    data_.request_candles(sym, ivl, rng);
}

// "" = a fresh built-in SMA; otherwise an instance from the module's factory.
IStrategy* App::acquire_strategy(const std::string& key) {
    if (key.empty()) return new SmaCrossover();
    return host_.create_instance(key);
}

void App::release_strategy(const StrategyLease& lease) {
    if (lease.key.empty())
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
    log_.add("backtest: building " + src + " first");
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
        log_.add("backtest: strategy build failed — see the Strategy panel");
    }
}

// ------------------------------------------------------------------ sweep

// UI thread: capture the request + strategy and fetch the data.
void App::queue_sweep(const SweepPanel::Request& rq) {
    if (!data_.connected()) {
        log_.add("sweep: feed is down, cannot fetch data");
        return;
    }
    if (sweep_.running || engine_.running()) {
        log_.add("sweep: engine busy, try again");
        return;
    }
    const std::string key = rq.strat_key;
    IStrategy* inst = acquire_strategy(key);
    if (!inst) {
        log_.add("sweep: strategy '" + strat_mgr_.display_name(key) + "' is not loaded");
        return;
    }
    leases_.push_back({inst, key, StrategyLease::Sweep});
    {
        std::lock_guard lock(pending_bt_mu_);
        sweep_setup_ = SweepSetup{};
        sweep_setup_.waiting = true;
        sweep_setup_.req = rq;
        sweep_setup_.strategy = inst;
        sweep_setup_.key = key;
        sweep_setup_.params = strat_mgr_.param_values(key);
    }
    data_.request_candles(rq.symbol, rq.interval, rq.range);
}

// IPC thread: if this batch is what the sweep is waiting for, stash the
// bars; the UI thread picks them up in pump_sweep().
void App::stash_pending_sweep(net::CandleBatch& batch) {
    std::lock_guard lock(pending_bt_mu_);
    if (!sweep_setup_.waiting || sweep_setup_.req.symbol != batch.symbol ||
        sweep_setup_.req.interval != batch.interval)
        return;
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
        log_.add("optimizer: engine busy, aborted");
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
            const SweepPanel::Request& rq = sweep_setup_.req;
            if (sweep_setup_.bars.size() < 3) {
                log_.add("sweep: not enough data for " + rq.symbol);
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
                    log_.add(static_cast<uint64_t>(cfg_.measured_lat_count) >= kMinLatSamples
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
                        log_.add("sweep: too little data for a holdout, skipped");
                    }
                }

                // Coordinate descent over the strategy's declared parameters,
                // starting from its current values.
                opt_ = AutoOpt{};
                opt_.key = sweep_setup_.key;
                // Sizing knobs are not signal: optimizing them just maximizes
                // leverage (the sim would happily oblige), so they keep their
                // manual values and only signal params are swept.
                auto is_sizing = [](const std::string& n) {
                    return n == "qty" || n == "max_qty" || n == "alloc_pct" ||
                           n == "risk_pct";
                };
                for (const auto& s : strat_mgr_.param_specs(opt_.key))
                    if (s.max > s.min && !is_sizing(s.name))
                        opt_.params.push_back({s.name, s.min, s.max});
                opt_.best = sweep_base_.params;

                sweep_ = SweepPanel::State{};
                sweep_.holdout_pct = holdout;
                sweep_.metric = rq.metric;
                sweep_.label = rq.symbol + " " + rq.interval + " " + rq.range + " — " +
                               strat_mgr_.display_name(sweep_setup_.key);
                if (opt_.params.empty()) {
                    log_.add("optimizer: no tunable parameters");
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

    if (!sweep_.running) return;
    // A mid-sweep rebuild is harmless now: the sweep's leased instance pins
    // its module until the last cell finishes.
    BacktestResult r;
    if (!engine_.take_result(r)) return;

    if (sweep_holdout_phase_) {   // the winner's run on unseen data
        sweep_.has_holdout = true;
        sweep_.holdout_val = sweep_metric_of(r, sweep_.metric);
        sweep_holdout_phase_ = false;
        sweep_.running = false;
        char buf[128];
        std::snprintf(buf, sizeof buf, "optimizer: holdout %s %.4g (last %.0f%%, unseen)",
                      kSweepMetrics[sweep_.metric], sweep_.holdout_val,
                      sweep_.holdout_pct);
        log_.add(buf);
        return;
    }

    sweep_.vals[static_cast<size_t>(opt_.step)] = sweep_metric_of(r, sweep_.metric);
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

    // All passes done: apply the winner to the strategy's parameters.
    strat_mgr_.set_param_values(opt_.key, opt_.best);
    sweep_.applied = true;
    std::string bests;
    for (const auto& [k, v] : opt_.best) {
        char kv[64];
        std::snprintf(kv, sizeof kv, "%s%s=%.4g", bests.empty() ? "" : " ", k.c_str(), v);
        bests += kv;
    }
    log_.add("optimizer: finished " + std::to_string(sweep_.done) + " backtests (" +
             sweep_.label + ") — applied " + bests);

    if (sweep_.holdout_pct <= 0 || sweep_test_bars_.empty()) {
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
        log_.add("optimizer: holdout run could not start (engine busy)");
        sweep_.running = false;
    }
}

// Optimize every loaded strategy (plus the built-in) on the same data; the
// champion (best HOLDOUT score — never in-sample) is applied to the target
// symbol's Trade tab. One candidate optimizes at a time through the normal
// sweep pipeline.
void App::start_tournament(SweepPanel::Request rq, const std::string& target_symbol,
                           std::vector<std::string> candidates) {
    if (tourn_.active || sweep_.running || engine_.running()) {
        log_.add("tournament: optimizer busy, try again");
        return;
    }
    // Champion selection needs unseen-data scoring; force a holdout.
    if (rq.holdout_pct <= 0) rq.holdout_pct = 25.0;
    tourn_ = Tournament{};
    tourn_.active = true;
    tourn_.base = std::move(rq);
    tourn_.target_symbol = target_symbol;
    if (candidates.empty()) {
        tourn_.candidates.push_back("");   // built-in SMA
        for (const std::string& k : strat_mgr_.loaded_keys())
            tourn_.candidates.push_back(k);
    } else {
        tourn_.candidates = std::move(candidates);
    }
    tourn_.stamp_s = ImGui::GetTime();

    sweep_.tourney = {};
    sweep_.tourney.active = true;
    sweep_.tourney.total = static_cast<int>(tourn_.candidates.size());
    sweep_.tourney.symbol = tourn_.base.symbol;
    log_.add("tournament: " + std::to_string(tourn_.candidates.size()) +
             " candidates on " + tourn_.base.symbol + " " + tourn_.base.interval + " " +
             tourn_.base.range);
}

void App::pump_tournament() {
    if (!tourn_.active) return;
    const double now = ImGui::GetTime();
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
        if (!sweep_.running && !engine_.running()) {
            SweepPanel::Request rq = tourn_.base;
            rq.strat_key = tourn_.candidates[tourn_.idx];
            queue_sweep(rq);
            // Keep the tournament banner alive (queue_sweep reset sweep_ state
            // when its candles arrive — re-assert below in Queued/Running).
            tourn_.phase = Tournament::Phase::Queued;
            tourn_.stamp_s = now;
        } else if (now - tourn_.stamp_s > 120.0) {
            log_.add("tournament: engine stayed busy, aborting");
            tourn_.active = false;
            sweep_.tourney.active = false;
        }
        break;
    case Tournament::Phase::Queued:
        sweep_.tourney.active = true;   // survive pump_sweep's sweep_ reset
        sweep_.tourney.total = static_cast<int>(tourn_.candidates.size());
        sweep_.tourney.idx = static_cast<int>(tourn_.idx);
        sweep_.tourney.symbol = tourn_.base.symbol;
        if (sweep_.running) {
            tourn_.phase = Tournament::Phase::Running;
        } else if (now - tourn_.stamp_s > 60.0) {
            // Candle fetch / strategy load failed (details in the log).
            Tournament::Entry e;
            e.key = tourn_.candidates[tourn_.idx];
            advance(std::move(e));
        }
        break;
    case Tournament::Phase::Running:
        if (sweep_.running) break;
        {
            Tournament::Entry e;
            e.key = tourn_.candidates[tourn_.idx];
            if (sweep_.has_best) {
                e.params = sweep_.best;
                e.holdout = sweep_.has_holdout;
                e.score = sweep_.has_holdout ? sweep_.holdout_val : sweep_.best_metric;
                e.valid = true;
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
        log_.add("tournament: no candidate produced a result");
        return;
    }
    const auto& c = tourn_.results[static_cast<size_t>(champ)];
    strat_mgr_.set_param_values(c.key, c.params);   // champion's params stick
    if (!tourn_.target_symbol.empty())
        trade_.set_symbol_strategy(tourn_.target_symbol, c.key, c.params);
    char buf[192];
    std::snprintf(buf, sizeof buf, "tournament: champion %s (%s %.4g%s) -> %s",
                  strat_mgr_.display_name(c.key).c_str(),
                  kSweepMetrics[tourn_.base.metric], c.score,
                  c.holdout ? " on holdout" : "", tourn_.target_symbol.c_str());
    log_.add(buf);
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
    const double now = ImGui::GetTime();

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
        static constexpr const char* kIvl[] = {"5m", "1h", "1d"};
        static constexpr const char* kRng[] = {"1mo", "6mo", "1y", "2y", "5y", "max"};
        SweepPanel::Request rq;
        rq.symbol = S.symbol;
        rq.interval = kIvl[std::clamp(st.interval_idx, 0, 2)];
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
        log_.add("autopilot: " + S.symbol + " cycle started (" +
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
        log_.add("autopilot: " + S.symbol + " cycle produced no result");
        return;
    }

    if (champ->key == S.key) {
        // Same strategy: refresh params only if they genuinely score better.
        if (!S.has_score || ap_better(ap_.metric, champ->score, S.incumbent_score)) {
            engine_.update_symbol_params(S.sid, champ->params);
            trade_.set_symbol_strategy(S.symbol, champ->key, champ->params);
            S.incumbent_score = champ->score;
            S.has_score = true;
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "autopilot: %s params queued (%s %.4g, applies when flat)",
                          S.symbol.c_str(), kSweepMetrics[ap_.metric], champ->score);
            log_.add(buf);
        } else {
            log_.add("autopilot: " + S.symbol + " kept (no improvement)");
        }
        S.challenger.clear();
        S.streak = 0;
        return;
    }

    // A different strategy won (full mode): swap only after it beats the
    // incumbent decisively twice in a row.
    if (S.has_score && !ap_better(ap_.metric, champ->score, S.incumbent_score)) {
        log_.add("autopilot: " + S.symbol + " challenger " +
                 strat_mgr_.display_name(champ->key) + " not decisive, kept " +
                 strat_mgr_.display_name(S.key));
        S.challenger.clear();
        S.streak = 0;
        return;
    }
    if (S.challenger != champ->key) {
        S.challenger = champ->key;
        S.streak = 1;
        log_.add("autopilot: " + S.symbol + " challenger " +
                 strat_mgr_.display_name(champ->key) + " (win 1/2)");
        return;
    }
    IStrategy* inst = acquire_strategy(champ->key);
    if (!inst) {
        log_.add("autopilot: " + S.symbol + " swap failed (strategy not loaded)");
        return;
    }
    leases_.push_back({inst, champ->key, StrategyLease::Live});
    engine_.swap_symbol_strategy(S.sid, inst, champ->params);
    trade_.set_symbol_strategy(S.symbol, champ->key, champ->params);
    S.key = champ->key;
    S.incumbent_score = champ->score;
    S.has_score = true;
    S.challenger.clear();
    S.streak = 0;
    log_.add("autopilot: " + S.symbol + " strategy swap queued -> " +
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
                   {"log", show_log_}};
    cfg_.strategy_loaded = strat_mgr_.loaded_keys();
    cfg_.strategy_params = strat_mgr_.all_param_values();
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
        log_.add("diag: endpoint disabled (diag_enabled=false in config.json)");
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
                log_.add("diag: REMOTE KILL-SWITCH requested via POST /control/kill");
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
        [this](std::string l) { log_.add(std::move(l)); });
    if (ok) {
        log_.add("diag: read-only endpoint on " + cfg_.diag_bind + ":" +
                 std::to_string(port) + "  —  browse  http://<tailscale-ip>:" +
                 std::to_string(port) + "/?token=" + cfg_.diag_token);
        if (cfg_.diag_control_enabled)
            log_.add("diag: REMOTE CONTROL ENABLED — POST /control/kill flattens + halts; "
                     "control token is in config.json (diag_control_token)");
    } else
        log_.add("diag: endpoint failed to start (is port " + std::to_string(port) +
                 " already in use?)");
}

// UI thread, per frame: re-render the /diag body at ~1 Hz into diag_json_, which
// the server thread copies out under diag_mu_.
void App::pump_diag() {
    // Execute a remote kill request here, on the UI thread — the only producer
    // allowed to touch the engine's SPSC command ring (the diag server thread
    // just set the flag).
    if (diag_kill_requested_.exchange(false, std::memory_order_relaxed)) {
        if (engine_.live_running()) {
            engine_.kill_switch();
            log_.add("diag: KILL-SWITCH EXECUTED — cancel all + flatten + halt");
        } else {
            log_.add("diag: kill-switch requested but no live session is running");
        }
    }
    if (!diag_srv_.running()) return;
    const double now = ImGui::GetTime();
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
    j["broker_connected"] = ibkr_ ? ibkr_->ready() : (tws_ ? tws_->ready() : false);

    // ---- session state ----
    j["live_running"] = s.running;
    j["halted"] = s.halted;
    j["cash"] = s.cash;
    j["equity"] = s.equity;
    j["ticks"] = s.ticks;
    j["dropped_ticks"] = s.dropped_ticks;
    j["feed_stale_ms"] = s.last_tick_ts_ms > 0 ? (now_ms - s.last_tick_ts_ms) : -1;
    j["stuck_orders"] = tws_ ? tws_->stuck_order_count() : 0;   // half-open (TWS route)

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
        const bool naked = ss.position.qty != 0.0 && !has_working_stop(ss.symbol);
        e["unprotected"] = naked;
        if (naked) ++unprotected;
        if (const TradeSymbol* ts = strat_for(ss.symbol)) {
            e["strategy"] = ts->strat_key.empty() ? "built-in SMA" : ts->strat_key;
            e["params"] = ts->params;
        }
        syms.push_back(std::move(e));
    }
    j["symbols"] = std::move(syms);
    j["unprotected_positions"] = unprotected;

    // ---- rejects feed (count + recent, with the broker's reject reason) ----
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
            r["reject_code"] = it->reject_code;   // 0 = no numeric code captured
            r["reject_msg"] = it->reject_msg;      // "" = no reason captured
            rejects.push_back(std::move(r));
        }
    j["reject_count"] = reject_count;
    j["rejects_recent"] = std::move(rejects);

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
    g("tt_broker_connected", "1 if the broker is connected",
      (ibkr_ ? ibkr_->ready() : (tws_ ? tws_->ready() : false)) ? 1 : 0);
    g("tt_equity_dollars", "account equity", s.equity);
    g("tt_cash_dollars", "account cash", s.cash);
    g("tt_ticks_total", "market-data ticks this session", static_cast<double>(s.ticks));
    g("tt_dropped_ticks_total", "ticks dropped (ring full)",
      static_cast<double>(s.dropped_ticks));
    g("tt_reject_count", "orders rejected this session", reject_count);
    g("tt_feed_stale_ms", "ms since the last tick (-1 = none yet)",
      s.last_tick_ts_ms > 0 ? static_cast<double>(now_ms - s.last_tick_ts_ms) : -1);
    g("tt_stuck_orders", "orders unacked past the half-open threshold (TWS)",
      tws_ ? tws_->stuck_order_count() : 0);
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

void App::draw() {
    const ImGuiID dockspace_id =
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
    if (ImGui::GetTime() - last_cfg_save_ > 60.0) {
        last_cfg_save_ = ImGui::GetTime();
        save_config();
    }
    pump_diag();   // re-render the /diag body (throttled) for the remote monitor
    if (!layout_checked_) {
        layout_checked_ = true;
        if (!had_ini_) setup_default_layout(dockspace_id);
    }

    // Surface engine/strategy/broker/feed log lines in the console; scan
    // them for alert-worthy events on the way through.
    std::string line;
    while (engine_.pop_log(line)) {
        alert_scan(line);
        log_.add(std::move(line));
    }
    if (ibkr_)
        while (ibkr_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (tws_)
        while (tws_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (tws_feed_)
        while (tws_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (polygon_feed_)
        while (polygon_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (finnhub_feed_)
        while (finnhub_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    if (ibkr_feed_)
        while (ibkr_feed_->pop_log(line)) {
            alert_scan(line);
            log_.add(std::move(line));
        }
    // Session over: stop streaming (frees the vendor connection slot).
    if ((polygon_feed_ || finnhub_feed_ || ibkr_feed_ || tws_feed_) &&
        !engine_.live_running()) {
        rt_feed_active_.store(false, std::memory_order_relaxed);
        polygon_feed_.reset();
        finnhub_feed_.reset();
        ibkr_feed_.reset();
        tws_feed_.reset();
        log_.add("live: real-time feed stopped");
    }

    // TT_AUTORUN_BACKTEST=1: fire one AAPL backtest as soon as the feed is up
    // (headless end-to-end verification of the UI -> data -> engine path).
    if (!autorun_bt_done_ && data_.connected() && std::getenv("TT_AUTORUN_BACKTEST")) {
        autorun_bt_done_ = true;
        queue_backtest("", "AAPL", "1d", "2y", 100'000.0);   // built-in SMA
        log_.add("autorun: queued AAPL 1d 2y backtest (built-in SMA)");
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
        log_.add("autorun: queued auto-optimize (built-in SMA)");
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
        journal_session_ = journal_.begin_session(
            jsyms, ibkr_ ? "ibkr" : "sim", engine_.live_snapshot().cash);
    } else if (prev_live_running_ && !live_now && journal_session_) {
        const LiveSnapshot s = engine_.live_snapshot();
        journal_.end_session(journal_session_, s.equity, s.halted);
        journal_session_ = 0;
    }
    prev_live_running_ = live_now;
    Engine::FillRecord fr;
    while (engine_.pop_fill(fr)) {
        const std::string sym = fr.symbol_id >= 1 && fr.symbol_id <= journal_syms_.size()
                                    ? journal_syms_[fr.symbol_id - 1]
                                    : "?";
        journal_.add_fill(journal_session_, fr.ts_ns, sym, fr.side == 1, fr.qty,
                          fr.price, fr.fee, fr.order_id);
    }

    pump_sweep();   // before the panels: sweep results must not be stolen
    pump_tournament();
    pump_autopilot();

    draw_menu_bar();
    draw_account_modal();
    draw_data_modal();
    draw_trading_guards();
    if (show_chart_) {
        // Overlay fills for the charted symbol: last backtest + live session.
        std::vector<FillMarker> fills;
        const std::string chart_sym = chart_.symbol();
        if (const BacktestResult* r = backtest_.result(); r && r->symbol == chart_sym)
            for (const TradeRow& t : r->fills)
                fills.push_back({static_cast<double>(t.ts_ns) / 1e9, t.price,
                                 t.side == static_cast<uint8_t>(Side::Buy)});
        if (engine_.live_running()) {
            const LiveSnapshot s = engine_.live_snapshot();
            for (const OrderRecord& o : s.orders)
                if (o.status == OrderStatus::Filled && o.symbol == chart_sym)
                    fills.push_back({static_cast<double>(o.ts_ns) / 1e9, o.fill_price,
                                     o.side == static_cast<uint8_t>(Side::Buy)});
        }
        chart_.draw(&show_chart_, fills);
    }
    if (show_watchlist_)
        watchlist_.draw(&show_watchlist_, [this](const std::string& sym) {
            chart_.show_symbol(sym);
            backtest_.set_symbol(sym);
        });
    // Deferred strategy loads, strategy-switch backtests, and finished-run
    // instance cleanup advance every frame, independent of open panels.
    strat_mgr_.pump();
    pump_pending_run();
    pump_leases();

    if (show_backtest_)
        backtest_.draw(&show_backtest_, strat_mgr_.sources(),
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
                             log_.add("replay: " + err);
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
                             log_.add("replay: strategy '" + strat_key + "' is not loaded");
                             return;
                         }
                         if (engine_.start_replay(std::move(cfg), strat)) {
                             // Replay shares the backtest engine slot/flag.
                             leases_.push_back({strat, strat_key, StrategyLease::Backtest});
                             log_.add("replay: running " + path + " (" +
                                      strat_mgr_.display_name(strat_key) + ")");
                         } else {
                             release_strategy({strat, strat_key, StrategyLease::Backtest});
                             log_.add("replay: engine busy, try again");
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
                              if (tourn_.active) {
                                  tourn_.active = false;
                                  sweep_.tourney.active = false;
                                  log_.add("tournament: cancelled");
                              } else {
                                  log_.add("sweep: cancelled");
                              }
                          });
    if (show_strategy_) strat_mgr_.draw(&show_strategy_);
    if (show_build_output_) strat_mgr_.draw_build_output(&show_build_output_);
    if (show_trade_)
        trade_.draw(&show_trade_, strat_mgr_.loaded_keys(),
                    [this](const std::string& k) {
                        std::vector<TradePanel::StratParam> out;
                        for (const auto& s : strat_mgr_.param_specs(k))
                            out.push_back({s.name, s.value, s.min, s.max});
                        return out;
                    },
                    [this](const std::string& k) { return strat_mgr_.display_name(k); },
                    [this](const std::string& sym) {
                        // Auto-pick: tournament on this symbol with the Optimizer
                        // panel's data settings; champion lands back in the tab.
                        const SweepPanel::Settings s = sweep_panel_.settings();
                        static constexpr const char* kIvl[] = {"5m", "1h", "1d"};
                        static constexpr const char* kRng[] = {"1mo", "6mo", "1y",
                                                               "2y",  "5y",  "max"};
                        SweepPanel::Request rq;
                        rq.symbol = sym;
                        rq.interval = kIvl[std::clamp(s.interval_idx, 0, 2)];
                        rq.range = kRng[std::clamp(s.range_idx, 0, 5)];
                        rq.cash = s.cash;
                        rq.metric = s.metric;
                        rq.holdout_pct = s.holdout ? s.holdout_pct : 25.0;
                        start_tournament(rq, sym);
                    },
                    !polygon_key().empty(), !finnhub_key().empty(), data_.connected(),
                    [&] {
                        TradePanel::AccountInfo a;
                        const auto ib = read_ibkr_accounts();
                        for (const auto& x : ib.accounts)
                            if (x.name == ib.active) { a.label = x.label; break; }
                        a.kind = static_cast<int>(data_.account_kind());
                        a.readonly = ib.active_readonly();
                        a.subaccounts = data_.accounts();
                        return a;
                    }(),
                    [this](const TradePanel::StartOpts& opts) {
                        std::vector<std::string> syms;
                        std::vector<int> sym_bars;
                        std::vector<std::string> sym_accts;
                        std::vector<RiskLimits> sym_risk;
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
                            any_record = any_record || so.record;
                            session_risk.daily_max_loss =
                                tight(session_risk.daily_max_loss, so.risk.daily_max_loss);
                            session_risk.max_drawdown_pct = tight(
                                session_risk.max_drawdown_pct, so.risk.max_drawdown_pct);
                            if (so.risk.stale_feed_sec > 0 &&
                                (session_risk.stale_feed_sec == 0 ||
                                 so.risk.stale_feed_sec < session_risk.stale_feed_sec))
                                session_risk.stale_feed_sec = so.risk.stale_feed_sec;
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
                            log_.add(ic.read_only
                                         ? "live: IBKR account is READ-ONLY — orders blocked"
                                         : "live: routing orders to IBKR gateway");
                        } else if (opts.broker == TradePanel::Broker::Tws) {
                            TwsConfig tc;
                            tc.port = tws_api_port();
                            tc.symbols = syms;
                            tc.symbol_accounts = sym_accts;
                            tc.read_only = read_ibkr_accounts().active_readonly();
                            const int port = tc.port;
                            tws_broker = std::make_unique<TwsBroker>(std::move(tc));
                            cfg.broker = tws_broker.get();
                            log_.add("live: routing orders via TWS socket (port " +
                                     std::to_string(port) + ")");
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
                            log_.add("live: building strategies (" + unbuilt +
                                     ") — click Start Trading again");
                            return;
                        }
                        // One strategy instance + param set per symbol.
                        std::vector<IStrategy*> strategies;
                        std::vector<StrategyLease> new_leases;
                        bool acq_ok = true;
                        for (const auto& so : opts.symbols) {
                            IStrategy* inst = acquire_strategy(so.strat_key);
                            if (!inst) {
                                log_.add("live: strategy '" +
                                         strat_mgr_.display_name(so.strat_key) +
                                         "' failed to load");
                                acq_ok = false;
                                break;
                            }
                            strategies.push_back(inst);
                            cfg.symbol_params.push_back(
                                so.params.empty() ? strat_mgr_.param_values(so.strat_key)
                                                   : so.params);
                            new_leases.push_back({inst, so.strat_key, StrategyLease::Live});
                        }
                        if (!acq_ok) {
                            for (const auto& l : new_leases) release_strategy(l);
                            return;
                        }
                        if (engine_.start_live(std::move(cfg), std::move(strategies))) {
                            for (const auto& l : new_leases) leases_.push_back(l);
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
                                aps.last_cycle_s = ImGui::GetTime();
                                ap_.syms.push_back(std::move(aps));
                            }
                            if (!ap_.syms.empty())
                                log_.add("autopilot: armed for " +
                                         std::to_string(ap_.syms.size()) + " symbol(s)");
                            // Previous session's thread was joined inside
                            // start_live, so replacing the old broker is safe.
                            ibkr_ = std::move(ibkr_broker);
                            tws_ = std::move(tws_broker);
                            polygon_feed_.reset();   // previous session's feeds
                            finnhub_feed_.reset();
                            ibkr_feed_.reset();
                            tws_feed_.reset();
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
                            log_.add("live: session queued for " + joined);
                        } else {
                            for (const auto& l : new_leases) release_strategy(l);
                            log_.add("live: cannot start (engine busy)");
                        }
                    });
    if (show_blotter_) blotter_.draw(&show_blotter_);
    if (show_positions_) positions_.draw(&show_positions_);
    if (show_journal_) journal_panel_.draw(&show_journal_);
    if (show_log_) log_.draw("Log Console", &show_log_);

#ifdef TT_DEBUG
    // Debug menu (or TT_SIM_TICKS=1): synthesize a 2 Hz random walk for the
    // live session — demo/verification when the market is closed.
    if (engine_.live_running() && sim_ticks_) {
        const double now = ImGui::GetTime();
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
    if (std::getenv("TT_AUTORUN_LIVE")) {
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
            log_.add("autorun-live: session started");
        } else if (autorun_live_stage_ == 1 && s.ticks >= 3) {
            autorun_live_stage_ = 2;
            engine_.submit_manual(1, true, 10);
            log_.add("autorun-live: manual BUY 10 SIMTEST submitted");
        } else if (autorun_live_stage_ == 2 && !s.symbols.empty() &&
                  s.symbols[0].position.qty > 0) {
            autorun_live_stage_ = 3;
            log_.add("autorun-live: position open, firing kill switch");
            engine_.kill_switch();
        } else if (autorun_live_stage_ == 3 && s.halted) {
            bool all_flat = true;
            for (const SymbolState& sym : s.symbols)
                if (sym.position.qty != 0) all_flat = false;
            if (all_flat) {
                autorun_live_stage_ = 4;
                log_.add("autorun-live: FLAT after kill switch — live path verified");
            }
        }
    }
    if (show_imgui_demo_) ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_implot_demo_) ImPlot::ShowDemoWindow(&show_implot_demo_);
}

void App::request_quit() {
    if (engine_.live_running()) pending_quit_ = true;   // draw_trading_guards() confirms
    else should_quit_ = true;
}

void App::safe_stop_live() {
    if (!engine_.live_running()) return;
    engine_.kill_switch();   // cancel all + flatten + halt strategy
    engine_.stop_live();     // graceful stop, joins the live thread
    log_.add("account: stopped live trading (kill switch)");
}

void App::do_ibkr_signout() {
    const std::string args = signout_args();
    if (!args.empty()) {
        run_hidden(args);
        log_.add("account: signing out of IBKR (stopping auto-login, gateway logout)");
    } else {
        log_.add("account: sign-out script missing (scripts\\Stop-IbkrLogin.ps1)");
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
                    log_.add("account: removing IBKR '" + acc.name + "'");
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
            log_.add("account: signed out of polygon");
        }
        if (!finn.empty() && ImGui::MenuItem(("finnhub: " + finn).c_str())) {
            accounts_.sign_out("finnhub");
            log_.add("account: signed out of finnhub");
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
                log_.add("account: removed '" + e.name + "'");
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// Route noteworthy engine/broker/feed log lines to the alert channel.
// Critical = money is at risk right now; Warning = something needs a look;
// Info = fills (webhook only, no beep — they can be frequent).
void App::alert_scan(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    if (has("KILL SWITCH") || has("RISK HALT") || has("WATCHDOG") ||
        has("PROTECTIVE STOP REJECTED"))
        alerts_.notify(AlertNotifier::Critical, l);
    else if (has("rejected") || has("stream lost") || has("auth failed") ||
             has("(drops!)") || has("half-open"))
        alerts_.notify(AlertNotifier::Warning, l);
    else if (has("live: fill"))
        alerts_.notify(AlertNotifier::Info, l);
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
    const bool initializing = !up && ImGui::GetTime() < gateway_starting_until_;

    // Kick off a sign-in (disconnected) or switch (connected, different
    // account) for `name`. Both run Switch-IbkrAccount, which tears the old
    // session down first. TWS route: that script only drives the CP web
    // gateway — switching means re-logging IB Gateway, done via script.
    auto do_switch = [&](const std::string& name) {
        if (use_tws_data_) {
            log_.add("account: TWS route - switch with scripts\\Start-IbGateway.ps1 "
                     "-Stop, then Start-IbGateway.ps1 -Account \"" + name + "\"");
            return;
        }
        const std::string args = switch_args(name);
        if (args.empty()) return;
        run_hidden(args);
        gateway_starting_until_ = ImGui::GetTime() + 90.0;
        log_.add(std::string("account: ") +
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
            log_.add("account: add IBKR account (enter credentials in the console)");
        } else {
            log_.add("account: Save-IbkrCred.ps1 not found");
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
        log_.add("account: signed in to " + a.provider + " as '" + a.name + "' (" +
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
        log_.add("account: saved " + a.provider + " '" + a.name + "' (not verified)");
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
        ImGui::Separator();
        bool alerts_on = !alerts_.muted();
        if (ImGui::MenuItem("Alerts", nullptr, &alerts_on)) alerts_.set_muted(!alerts_on);
        ImGui::SetItemTooltip(alerts_.has_webhook()
                                  ? "Beep + webhook on halts, rejects, disconnects, fills"
                                  : "Beeps on halts/rejects/disconnects. Set "
                                    "\"alert_webhook\" in config.json (or "
                                    "TT_ALERT_WEBHOOK) for phone push, e.g. an "
                                    "ntfy.sh topic URL");
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
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo_);
        ImGui::MenuItem("ImPlot Demo", nullptr, &show_implot_demo_);
        ImGui::EndMenu();
    }

    // Right-aligned gateway session indicator.
    const bool up = data_.connected();
    const bool initializing = !up && ImGui::GetTime() < gateway_starting_until_;
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
    ImGui::DockBuilderDockWindow("Log Console", bottom);
    ImGui::DockBuilderDockWindow("Blotter", bottom);
    ImGui::DockBuilderDockWindow("Positions", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace tt::ui
