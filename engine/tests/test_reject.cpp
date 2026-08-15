// NO REJECTION WITHOUT A REASON.
//
// 2026-08-13: /diag showed two rejected orders, both with reject_code 0 and an
// empty reject_msg. Nothing in the record said whether a local risk check, a
// buying-power gate, a notional clamp or IBKR itself had refused them, and the
// operator (and the investigation that followed) had to reconstruct the answer
// from a 12,000-line log. These tests exist so that record cannot happen again:
//
//   1. THE GAP TEST walks every enumerator of RejectCause and fails if any one
//      of them lacks a slug or a sentence. Adding an enumerator without both
//      breaks the suite rather than shipping a blank reason.
//   2. THE INVARIANT TEST drives real live sessions down each locally reachable
//      refusal path and asserts that EVERY rejected order in the resulting
//      snapshot carries a cause != None and a non-empty message.

#include "doctest.h"

#include "engine/broker.h"
#include "engine/engine.h"
#include "engine/reject.h"

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace tt;

// ---- 1. the taxonomy itself (pure) -----------------------------------------

TEST_CASE("reject taxonomy: kAllRejectCauses covers the whole enum") {
    // Catches the half-update: a new enumerator added to the enum but not to the
    // array the rest of these tests (and any future consumer) walk.
    CHECK(std::size(kAllRejectCauses) == static_cast<size_t>(RejectCause::Count_));
    // ...and that the array is the enum in order, so index == value. Several
    // call sites treat the two interchangeably.
    for (size_t i = 0; i < std::size(kAllRejectCauses); ++i)
        CHECK(static_cast<uint16_t>(kAllRejectCauses[i]) == static_cast<uint16_t>(i));
}

TEST_CASE("reject taxonomy: every cause has a slug and a sentence") {
    // THE GAP TEST. A missing case in either switch returns "" and fails here.
    for (RejectCause c : kAllRejectCauses) {
        const std::string slug = reject_cause_slug(c);
        const std::string text = reject_cause_text(c);
        INFO("cause index ", static_cast<int>(c), " slug='", slug, "'");
        CHECK(!slug.empty());
        CHECK(!text.empty());
    }
}

TEST_CASE("reject taxonomy: slugs and sentences are unique") {
    // Two causes sharing a slug would silently merge in /diag, in the refusal
    // table's (symbol, cause) key and in any grep the operator runs.
    std::set<std::string> slugs, texts;
    for (RejectCause c : kAllRejectCauses) {
        CHECK(slugs.insert(reject_cause_slug(c)).second);
        CHECK(texts.insert(reject_cause_text(c)).second);
    }
}

TEST_CASE("reject taxonomy: sentences are pure ASCII") {
    // A refusal can page, and AlertNotifier's webhook body must be ASCII or
    // ntfy serves it as a download instead of a message (see terminal/alerts.h).
    for (RejectCause c : kAllRejectCauses)
        for (const char* p = reject_cause_text(c); *p; ++p)
            CHECK(static_cast<unsigned char>(*p) < 0x80);
}

TEST_CASE("reject taxonomy: broker causes are exactly the venue's own") {
    // The operator question /diag could not answer at all on 2026-08-13: is this
    // us or is this them?
    CHECK(reject_cause_is_broker(RejectCause::BrokerRejected));
    CHECK(reject_cause_is_broker(RejectCause::BrokerNotConnected));
    CHECK(reject_cause_is_broker(RejectCause::BrokerRefused));
    CHECK_FALSE(reject_cause_is_broker(RejectCause::NotionalCap));
    CHECK_FALSE(reject_cause_is_broker(RejectCause::SessionClosed));
    CHECK_FALSE(reject_cause_is_broker(RejectCause::None));
}

TEST_CASE("reject taxonomy: exactly two causes refuse an exit by design") {
    // What /diag's refused_exits and tt_refused_exits subtract before they claim
    // "there is no benign value but 0". Adding a third by-design refusal without
    // thinking about that claim should break here first.
    CHECK(reject_cause_exit_is_by_design(RejectCause::HoldBlocksLoss));
    CHECK(reject_cause_exit_is_by_design(RejectCause::WarmupReplay));
    int by_design = 0;
    for (RejectCause c : kAllRejectCauses)
        if (reject_cause_exit_is_by_design(c)) ++by_design;
    CHECK(by_design == 2);
    // The ones that must never be excused: each is a position left with one
    // fewer thing that will close it.
    CHECK_FALSE(reject_cause_exit_is_by_design(RejectCause::BrokerRejected));
    CHECK_FALSE(reject_cause_exit_is_by_design(RejectCause::BrokerNotConnected));
    CHECK_FALSE(reject_cause_exit_is_by_design(RejectCause::PriceBand));
    CHECK_FALSE(reject_cause_exit_is_by_design(RejectCause::MaxOrderQty));
}

TEST_CASE("reject taxonomy: the alert tags nest, so classify_alert order matters") {
    // Both specific tags contain the generic one as a substring, which is why
    // tt::ui::classify_alert must test Critical and Repeat BEFORE the plain tag.
    // Pinned here so a reworded tag cannot silently downgrade an exit refusal
    // from Critical to Info.
    const std::string generic = kOrderRefusedTag;
    CHECK(std::string(kExitOrderRefusedTag).find(generic) != std::string::npos);
    CHECK(std::string(kOrderRefusedRepeatTag).find(generic) != std::string::npos);
}

// ---- 2. the adapter defaults ------------------------------------------------

namespace {
// An adapter that implements nothing about reasons: the shape every future
// adapter starts as. It must still produce a usable answer.
struct SilentBroker : IBrokerAdapter {
    uint64_t submit(const OrderRequest&, int64_t) override { return 0; }
    bool cancel(uint64_t) override { return false; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent&) override { return false; }
    bool ready() const override { return true; }
};
} // namespace

TEST_CASE("broker: the default take_reject still gives the operator a sentence") {
    // INVERTED in 0.23.0. This used to assert the default was blank, and blank
    // is exactly what reached /diag on 2026-08-13 - the base class was pinning
    // the defect in place. An adapter that tracks no reasons owes a generic
    // answer, not silence.
    SilentBroker b;
    const RejectReason r = b.take_reject(42);
    CHECK(r.cause == RejectCause::BrokerRefused);
    CHECK_FALSE(r.message.empty());
    CHECK_FALSE(r.empty());
}

TEST_CASE("broker: the default last_submit_reject gives the operator a sentence") {
    SilentBroker b;
    const RejectReason r = b.last_submit_reject();
    CHECK(r.cause != RejectCause::None);
    CHECK_FALSE(r.message.empty());
}

// ---- 3. the live invariant --------------------------------------------------

namespace {

// Submits one caller-supplied order on the first tick (or on on_init, for the
// paths that must be reached before any price exists), then goes quiet.
struct OneShotStrat : IStrategy {
    OrderRequest req{};
    bool on_init_ = false;
    std::atomic<bool> sent{false};
    std::atomic<uint64_t> last_id{0};

    void on_init(IStrategyContext& ctx) noexcept override {
        if (on_init_ && !sent.exchange(true)) last_id = ctx.submit_order(req);
    }
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t, const Tick&) noexcept override {
        if (!on_init_ && !sent.exchange(true)) last_id = ctx.submit_order(req);
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};

// Refuses every submit with a caller-chosen cause: the "submit() returned 0"
// half of the plumbing, which used to record a bare 0 in the blotter.
struct RefusingBroker : IBrokerAdapter {
    RejectReason why;
    explicit RefusingBroker(RejectCause c, std::string m)
        : why{c, 0, std::move(m)} {}
    uint64_t submit(const OrderRequest&, int64_t) override { return 0; }
    bool cancel(uint64_t) override { return false; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent&) override { return false; }
    bool ready() const override { return true; }
    RejectReason last_submit_reject() const override { return why; }
};

// Accepts, then asynchronously rejects - the production shape (IBKR acks the
// order, then reports it Inactive). `reason` empty = the adapter says nothing,
// which is the 2026-08-13 case the engine must now backfill.
struct LateRejectBroker : IBrokerAdapter {
    std::mutex mu;
    std::deque<EngineEvent> q;
    uint64_t next = 1;
    RejectReason reason{};
    bool have_reason = false;

    uint64_t submit(const OrderRequest& r, int64_t) override {
        std::lock_guard l(mu);
        const uint64_t id = next++;
        EngineEvent e{};
        e.type = static_cast<uint16_t>(EvType::OrderCancel);
        e.flags = kEvFlagRejected;
        e.symbol_id = r.symbol_id;
        e.u.order.order_id = id;
        q.push_back(e);
        return id;
    }
    bool cancel(uint64_t) override { return true; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent& out) override {
        std::lock_guard l(mu);
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    bool ready() const override { return true; }
    RejectReason take_reject(uint64_t) override {
        return have_reason ? reason : RejectReason{};
    }
};

bool pump(Engine& eng, std::function<bool()> pred, int ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        eng.push_live_tick("AAA", 1, 50.0, 0.0);
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return pred();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

OrderRequest mkt(Side side, double qty, uint32_t sid = 1) {
    OrderRequest r{};
    r.symbol_id = sid;
    r.side = side;
    r.type = OrdType::Market;
    r.qty = qty;
    return r;
}

// THE INVARIANT, asserted on every snapshot every one of these cases produces:
// a rejected order always says why, and the refusal table never holds a
// cause-less or wordless row.
void check_no_blank_reasons(const LiveSnapshot& s) {
    for (const OrderRecord& o : s.orders) {
        if (o.status != OrderStatus::Rejected) continue;
        INFO("rejected order #", o.id, " ", o.symbol);
        CHECK(o.reject_cause != RejectCause::None);
        CHECK_FALSE(o.reject_msg.empty());
    }
    for (const RefusalStat& r : s.refusals) {
        INFO("refusal row ", r.symbol, " ", std::string(reject_cause_slug(r.cause)));
        CHECK(r.cause != RejectCause::None);
        CHECK_FALSE(r.message.empty());
        CHECK(r.count > 0);
    }
}

// Runs one live session that submits `req` under `tune`, and returns the
// snapshot after the refusal has been observed (or the timeout).
LiveSnapshot run_one(const OrderRequest& req, RejectCause expect,
                     void (*tune)(LiveConfig&) = nullptr, bool from_init = false) {
    Engine eng;
    OneShotStrat strat;
    strat.req = req;
    strat.on_init_ = from_init;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;   // no bars fire; drive the on_tick path
    cfg.initial_cash = 100'000.0;
    if (tune) tune(cfg);
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] {
        const LiveSnapshot s = eng.live_snapshot();
        for (const RefusalStat& r : s.refusals)
            if (r.cause == expect) return true;
        return false;
    });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    return s;
}

bool has_cause(const LiveSnapshot& s, RejectCause c) {
    for (const RefusalStat& r : s.refusals)
        if (r.cause == c) return true;
    return false;
}

const RefusalStat* row(const LiveSnapshot& s, RejectCause c) {
    for (const RefusalStat& r : s.refusals)
        if (r.cause == c) return &r;
    return nullptr;
}

} // namespace

TEST_CASE("refusal: a zero-quantity order is named, not silently dropped") {
    // engine.cpp used to `return 0` here with no record and no log at all -
    // invisible to /diag AND to the alert channel simultaneously.
    const LiveSnapshot s = run_one(mkt(Side::Buy, 0.0), RejectCause::InvalidQty);
    CHECK(has_cause(s, RejectCause::InvalidQty));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: an order for a symbol outside the session is named") {
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 10.0, /*sid=*/9), RejectCause::SymbolNotInSession);
    CHECK(has_cause(s, RejectCause::SymbolNotInSession));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a limit order with no limit price is named") {
    OrderRequest r = mkt(Side::Buy, 10.0);
    r.type = OrdType::Limit;
    const LiveSnapshot s = run_one(r, RejectCause::MissingLimitPrice);
    CHECK(has_cause(s, RejectCause::MissingLimitPrice));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a stop order with no trigger price is named") {
    OrderRequest r = mkt(Side::Buy, 10.0);
    r.type = OrdType::Stop;
    const LiveSnapshot s = run_one(r, RejectCause::MissingStopPrice);
    CHECK(has_cause(s, RejectCause::MissingStopPrice));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: over the per-order share limit is named as such") {
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 500.0), RejectCause::MaxOrderQty, [](LiveConfig& c) {
            c.risk.max_order_qty = 10;
        });
    CHECK(has_cause(s, RejectCause::MaxOrderQty));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: over the per-symbol position limit is named as such") {
    // Distinct from MaxOrderQty: both used to share one bare code-0 rejection,
    // so /diag could not tell an oversized order from an oversized position.
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 500.0), RejectCause::MaxPositionQty, [](LiveConfig& c) {
            c.risk.max_order_qty = 1000;
            c.risk.max_position_qty = 10;
        });
    CHECK(has_cause(s, RejectCause::MaxPositionQty));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a fat-fingered limit price is named as such") {
    OrderRequest r = mkt(Side::Buy, 10.0);
    r.type = OrdType::Limit;
    r.limit_price = 5000.0;   // last trade is 50
    const LiveSnapshot s = run_one(r, RejectCause::PriceBand, [](LiveConfig& c) {
        c.risk.price_band_pct = 0.20;
    });
    CHECK(has_cause(s, RejectCause::PriceBand));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a position already at its dollar cap is named as such") {
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 100.0), RejectCause::NotionalCap, [](LiveConfig& c) {
            c.risk.max_position_notional = 10.0;   // < one share at 50
        });
    CHECK(has_cause(s, RejectCause::NotionalCap));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: an order with no usable price is refused, not sent unsized") {
    // PART 2's "make it impossible rather than reject it later". Submitted from
    // on_init, before any tick has set a price. clamp_to_notional used to
    // `return` here and let the order through at FULL SIZE with the dollar cap
    // silently unapplied.
    const LiveSnapshot s = run_one(mkt(Side::Buy, 100.0), RejectCause::PriceUnavailable,
                                   [](LiveConfig& c) {
                                       c.risk.max_position_notional = 5000.0;
                                   },
                                   /*from_init=*/true);
    CHECK(has_cause(s, RejectCause::PriceUnavailable));
    check_no_blank_reasons(s);
    // ...and nothing reached the fill simulator.
    for (const OrderRecord& o : s.orders) CHECK(o.status == OrderStatus::Rejected);
}

TEST_CASE("refusal: exceeding simulated buying power is named as such") {
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 100.0), RejectCause::BuyingPower, [](LiveConfig& c) {
            c.initial_cash = 100.0;   // 100 shares at 50 is far beyond it
        });
    CHECK(has_cause(s, RejectCause::BuyingPower));
    check_no_blank_reasons(s);
}

TEST_CASE("entry gate: a closed session refuses an ENTRY with a named cause") {
    // The 2026-08-13 order, reproduced against a gate that says "shut".
    const LiveSnapshot s =
        run_one(mkt(Side::Buy, 100.0), RejectCause::SessionClosed, [](LiveConfig& c) {
            c.entry_gate = [](int64_t) { return false; };
        });
    CHECK(has_cause(s, RejectCause::SessionClosed));
    CHECK(s.entry_gate_armed);
    CHECK_FALSE(s.entry_gate_open);
    CHECK(s.entry_gate_blocked >= 1);
    // An entry is not an exit, so it must NOT have been counted as one - that
    // distinction is what keeps the Critical tier meaningful.
    const RefusalStat* r = row(s, RejectCause::SessionClosed);
    REQUIRE(r != nullptr);
    CHECK_FALSE(r->exit_order);
    check_no_blank_reasons(s);
}

TEST_CASE("entry gate: an unarmed session says so in the snapshot") {
    // The silent-failure mode: the app forgot to install a gate. It must read
    // differently from an armed one that happens to be open.
    Engine eng;
    OneShotStrat strat;
    strat.req = mkt(Side::Buy, 1.0);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] { return strat.sent.load(); });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    CHECK_FALSE(s.entry_gate_armed);
    // Disarmed reads OPEN, because nothing is being blocked - `armed` is the
    // field that distinguishes the two, and that is why both are published.
    CHECK(s.entry_gate_open);
    CHECK(s.entry_gate_blocked == 0);
}

TEST_CASE("entry gate: a shut gate never blocks an EXIT") {
    // Refusing to close is how positions go naked. The gate must be incapable of
    // it, so this drives a real fill and then a real exit through a shut gate.
    Engine eng;
    struct BuyThenSell : IStrategy {
        std::atomic<int> stage{0};
        std::atomic<uint64_t> exit_id{0};
        void on_init(IStrategyContext&) noexcept override {}
        void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
        void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
            if (stage.load() == 0) {
                stage = 1;
                OrderRequest r{};
                r.symbol_id = sid;
                r.side = Side::Buy;
                r.type = OrdType::Market;
                r.qty = 10;
                ctx.submit_order(r);
            } else if (stage.load() == 2) {
                stage = 3;
                OrderRequest r{};
                r.symbol_id = sid;
                r.side = Side::Sell;
                r.type = OrdType::Market;
                r.qty = 10;
                exit_id = ctx.submit_order(r);
            }
        }
        void on_fill(IStrategyContext&, const Fill&) noexcept override {
            int expect = 1;
            stage.compare_exchange_strong(expect, 2);
        }
        void on_stop(IStrategyContext&) noexcept override {}
        void destroy() noexcept override {}
    } strat;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    // Shut from the very first instant: the BUY is refused, so open the gate for
    // the entry and shut it before the exit instead.
    std::atomic<bool> open{true};
    cfg.entry_gate = [&open](int64_t) { return open.load(); };
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] { return strat.stage.load() >= 2; });
    open = false;   // exchange "closes" between the entry and the exit
    pump(eng, [&] { return strat.exit_id.load() != 0; });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    CHECK(strat.exit_id.load() != 0);              // the exit was PLACED
    CHECK_FALSE(has_cause(s, RejectCause::SessionClosed));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a broker that refuses a submit says which of its four reasons") {
    RefusingBroker broker(RejectCause::BrokerNotConnected,
                          "the order path is not connected");
    Engine eng;
    OneShotStrat strat;
    strat.req = mkt(Side::Buy, 10.0);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] { return has_cause(eng.live_snapshot(), RejectCause::BrokerNotConnected); });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    CHECK(has_cause(s, RejectCause::BrokerNotConnected));
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a reason-less broker reject is backfilled, never left blank") {
    // THE 2026-08-13 RECORD, reproduced: the adapter acks the order and then
    // rejects it having captured nothing. /diag printed code 0 and "".
    LateRejectBroker broker;   // have_reason = false
    Engine eng;
    OneShotStrat strat;
    strat.req = mkt(Side::Buy, 10.0);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] {
        for (const OrderRecord& o : eng.live_snapshot().orders)
            if (o.status == OrderStatus::Rejected) return true;
        return false;
    });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    bool saw = false;
    for (const OrderRecord& o : s.orders)
        if (o.status == OrderStatus::Rejected) {
            saw = true;
            CHECK(o.reject_cause == RejectCause::BrokerRefused);
            CHECK_FALSE(o.reject_msg.empty());
        }
    CHECK(saw);
    check_no_blank_reasons(s);
}

TEST_CASE("refusal: a broker reject WITH a reason keeps the broker's code and text") {
    // The other half: when IB does say "201 Exchange is closed", that must
    // survive into the record rather than being replaced by the generic.
    LateRejectBroker broker;
    broker.have_reason = true;
    broker.reason = {RejectCause::BrokerRejected, 201,
                     "Order rejected - reason:Exchange is closed."};
    Engine eng;
    OneShotStrat strat;
    strat.req = mkt(Side::Buy, 10.0);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] {
        for (const OrderRecord& o : eng.live_snapshot().orders)
            if (o.status == OrderStatus::Rejected) return true;
        return false;
    });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    bool saw = false;
    for (const OrderRecord& o : s.orders)
        if (o.status == OrderStatus::Rejected) {
            saw = true;
            CHECK(o.reject_cause == RejectCause::BrokerRejected);
            CHECK(o.reject_code == 201);
            CHECK(o.reject_msg.find("Exchange is closed") != std::string::npos);
        }
    CHECK(saw);
    check_no_blank_reasons(s);
}

TEST_CASE("refusal table: repeats are counted, not turned into 200 blotter rows") {
    // The flood guard. A strategy retrying into the same gate every tick would
    // otherwise evict every real order from a 200-entry ring - instrumentation
    // drowning the truth it exists to preserve.
    struct SpamStrat : IStrategy {
        std::atomic<int> tries{0};
        void on_init(IStrategyContext&) noexcept override {}
        void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
        void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
            ++tries;
            OrderRequest r{};
            r.symbol_id = sid;
            r.side = Side::Buy;
            r.type = OrdType::Market;
            r.qty = 10;
            ctx.submit_order(r);
        }
        void on_fill(IStrategyContext&, const Fill&) noexcept override {}
        void on_stop(IStrategyContext&) noexcept override {}
        void destroy() noexcept override {}
    } strat;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.entry_gate = [](int64_t) { return false; };
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] {
        const RefusalStat* r = row(eng.live_snapshot(), RejectCause::SessionClosed);
        return r && r->count >= 20;
    });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    const RefusalStat* r = row(s, RejectCause::SessionClosed);
    REQUIRE(r != nullptr);
    CHECK(r->count >= 20);                 // every one counted
    CHECK(r->last_ts_ns > r->first_ts_ns); // and the row says it is ongoing
    int rejected_rows = 0;
    for (const OrderRecord& o : s.orders)
        if (o.status == OrderStatus::Rejected) ++rejected_rows;
    CHECK(rejected_rows == 1);             // ...but only one blotter row
    CHECK(s.entry_gate_blocked >= 20);
    check_no_blank_reasons(s);
}

TEST_CASE("alerting: a refused EXIT emits the Critical tag, an entry does not") {
    // The load-bearing half of the policy, end to end. tt::ui::classify_alert
    // rates kExitOrderRefusedTag Critical (terminal/tests/test_alerts.cpp); this
    // proves the engine actually EMITS that tag when an order that would have
    // closed a position is refused, so the two halves cannot drift apart.
    //
    // Buys 10 at 50 (allowed), then offers its exit as a limit at 5000, which
    // the fat-finger price band refuses. An exit is the one refusal with no
    // benign reading: the position stays open with one fewer thing closing it.
    struct BuyThenBadExit : IStrategy {
        std::atomic<int> stage{0};
        void on_init(IStrategyContext&) noexcept override {}
        void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
        void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
            OrderRequest r{};
            r.symbol_id = sid;
            r.qty = 10;
            if (stage.load() == 0) {
                stage = 1;
                r.side = Side::Buy;
                r.type = OrdType::Market;
                ctx.submit_order(r);
            } else if (stage.load() == 2) {
                stage = 3;
                r.side = Side::Sell;
                r.type = OrdType::Limit;
                r.limit_price = 5000.0;   // last trade is 50: way outside the band
                ctx.submit_order(r);
            }
        }
        void on_fill(IStrategyContext&, const Fill&) noexcept override {
            int expect = 1;
            stage.compare_exchange_strong(expect, 2);
        }
        void on_stop(IStrategyContext&) noexcept override {}
        void destroy() noexcept override {}
    } strat;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.risk.price_band_pct = 0.20;
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] { return has_cause(eng.live_snapshot(), RejectCause::PriceBand); });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();

    const RefusalStat* r = row(s, RejectCause::PriceBand);
    REQUIRE(r != nullptr);
    CHECK(r->exit_order);   // it would have SHRUNK an open position
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(kExitOrderRefusedTag) != std::string::npos);
    // ...and it is the Critical tag, not the ordinary one used on its own.
    const size_t at = all.find(kOrderRefusedTag);
    REQUIRE(at != std::string::npos);
    CHECK(all.compare(at - 5, 5, "EXIT ") == 0);
    check_no_blank_reasons(s);
}

// ---- 4. the refusal that says nothing ---------------------------------------
// Everything below is 0.23.1, and every case here PASSED SILENTLY in 0.23.0 for
// the same reason: the alerting policy was keyed on "is this the first row for
// this (symbol, cause) pair", which is not the question any of it was asking.

namespace {
// Refused entry, then a real fill, then a refused EXIT - all on ONE (symbol,
// cause) pair. The shape that goes silent: any cause reachable by both an entry
// and an exit (broker_not_connected during a gateway outage, broker_rejected on
// an IB 201 in the morning and again in the afternoon, max_order_qty after a
// limit change) hits it.
struct EntryRefusedThenExitRefused : IStrategy {
    std::atomic<int> stage{0};       // 0 bad entry, 1 real buy, 3 bad exit
    std::atomic<int> bad_exits{0};
    std::atomic<int> bad_entries{0};
    bool keep_trying = false;        // stay on the entry path after the exit
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
    void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
        OrderRequest r{};
        r.symbol_id = sid;
        r.qty = 10;
        const int st = stage.load();
        if (st == 0) {
            stage = 1;
            r.side = Side::Buy;              // flat, so this is an ENTRY
            r.type = OrdType::Limit;
            r.limit_price = 5000.0;          // last trade is 50: outside the band
            ctx.submit_order(r);
            ++bad_entries;
        } else if (st == 1) {
            stage = 2;                       // wait for the fill (on_fill -> 3)
            r.side = Side::Buy;
            r.type = OrdType::Market;
            ctx.submit_order(r);
        } else if (st == 3) {
            stage = keep_trying ? 4 : 5;
            r.side = Side::Sell;             // long 10, so this SHRINKS it: EXIT
            r.type = OrdType::Limit;
            r.limit_price = 5000.0;
            ctx.submit_order(r);
            ++bad_exits;
        } else if (st == 4) {
            stage = 5;
            r.side = Side::Sell;             // a second refused exit
            r.type = OrdType::Limit;
            r.limit_price = 5000.0;
            ctx.submit_order(r);
            ++bad_exits;
        } else if (st == 5 && keep_trying) {
            r.side = Side::Buy;              // long 10, so buying ADDS: entry
            r.type = OrdType::Limit;
            r.limit_price = 5000.0;
            ctx.submit_order(r);
            ++bad_entries;
        }
    }
    void on_fill(IStrategyContext&, const Fill&) noexcept override {
        int expect = 2;
        stage.compare_exchange_strong(expect, 3);
    }
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

TEST_CASE("alerting: a refused EXIT pages even when an ENTRY was refused first") {
    // THE BLOCKER of the 0.23.0 review. The Critical tag was emitted only when
    // the refusal created the (symbol, cause) row, and that row is SHARED with
    // entry refusals - so one harmless entry refusal earlier in the session
    // (the entry gate declining a buy at 16:05) bought permanent silence for
    // every refused exit on that symbol for that cause. No Critical, no
    // Warning, not even the Info line: the exit branch returned first.
    //
    // Alerting is entirely log-line driven (App::alert_scan -> classify_alert),
    // so no line means no page of ANY tier, while the position that exit would
    // have closed stays open.
    EntryRefusedThenExitRefused strat;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 100'000.0;
    cfg.risk.price_band_pct = 0.20;
    REQUIRE(eng.start_live(cfg, {&strat}));
    // Wait for the ENGINE to have PUBLISHED the exit refusal, not merely for the
    // strategy to have submitted it. bad_exits is bumped on the engine thread the
    // instant submit_order returns 0, but live_snapshot() serves a snapshot the
    // engine republishes on its own cadence, so the two are different events.
    // Waiting on the strategy's counter and then reading the snapshot failed 2
    // runs in 10 of the RELEASE build, always the same way: the snapshot held the
    // entry refusal only (count 1, exit_count 0, one Rejected row), because it
    // was published between the two refusals. Debug never showed it.
    LiveSnapshot s;
    pump(eng, [&] {
        s = eng.live_snapshot();
        const RefusalStat* pending = row(s, RejectCause::PriceBand);
        return pending != nullptr && pending->exit_count >= 1;
    });
    eng.stop_live();

    const RefusalStat* r = row(s, RejectCause::PriceBand);
    REQUIRE(r != nullptr);
    CHECK(r->count >= 2);        // the entry and the exit share the row...
    CHECK(r->exit_count == 1);   // ...but only ONE of them was an exit
    CHECK(r->exit_order);
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(kExitOrderRefusedTag) != std::string::npos);
    // The blotter is what /diag's reject_count and rejects_recent are built
    // from, and it recorded only the first row per pair - so the refused EXIT,
    // the one the operator needs, was missing from there too.
    int rejected_rows = 0;
    for (const OrderRecord& o : s.orders)
        if (o.status == OrderStatus::Rejected) ++rejected_rows;
    CHECK(rejected_rows >= 2);
    check_no_blank_reasons(s);
}

TEST_CASE("alerting: the repeat Warning cannot be swallowed by a silent branch") {
    // The counter advances for EVERY refusal, including the branches that return
    // before the repeat check (an exit, a hold_blocks_loss, a warmup replay).
    // Tested with ==, the threshold value gets consumed inside one of those and
    // the Warning then NEVER fires for that pair again, however many times the
    // strategy jams. Here refusals 2 and 3 are exits, so 0.23.0's `count ==
    // kRefusalRepeatAlertAt` never matched on the entry path and a strategy
    // refused a dozen times running said nothing.
    EntryRefusedThenExitRefused strat;
    strat.keep_trying = true;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 100'000.0;
    cfg.risk.price_band_pct = 0.20;
    REQUIRE(eng.start_live(cfg, {&strat}));
    // Snapshot-driven for the same reason as the exit-refusal case above: these
    // assertions read the PUBLISHED counters, so waiting on the strategy's own
    // tally can hand us a snapshot taken between two refusals. exit_count == 2 is
    // an exact equality, which is the shape that turns that race into a failure.
    LiveSnapshot s;
    pump(eng, [&] {
        s = eng.live_snapshot();
        const RefusalStat* pending = row(s, RejectCause::PriceBand);
        return pending != nullptr && pending->exit_count >= 2 &&
               pending->count > kRefusalRepeatAlertAt;
    });
    eng.stop_live();

    const RefusalStat* r = row(s, RejectCause::PriceBand);
    REQUIRE(r != nullptr);
    CHECK(r->count > kRefusalRepeatAlertAt);
    CHECK(r->exit_count == 2);
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(kOrderRefusedRepeatTag) != std::string::npos);
}

TEST_CASE("alerting: hold-until-profitable refuses exits all day and never pages") {
    // THE ONE DELIBERATE EXCEPTION, and the one this fix could most easily have
    // broken. hold mode refuses exits BY DESIGN - that is what
    // hold-until-profitable IS - and the strategy re-offers the same exit every
    // tick. Now that a refused exit repeats its Critical page on a backoff, a
    // hold_blocks_loss refusal falling through to the exit branch would page
    // every five minutes for every held position, all day. It says so once
    // (Info) and is then visible only in the refusal table.
    //
    // /diag's refused_exits and tt_refused_exits skip this cause for the same
    // reason: both are documented "there is no benign value but 0", and one
    // held position produced 189 before 0.23.1.
    struct BuyThenHeldExit : IStrategy {
        std::atomic<int> stage{0};
        std::atomic<int> refused{0};
        void on_init(IStrategyContext&) noexcept override {}
        void on_bar(IStrategyContext&, uint32_t, const Bar&) noexcept override {}
        void on_tick(IStrategyContext& ctx, uint32_t sid, const Tick&) noexcept override {
            OrderRequest r{};
            r.symbol_id = sid;
            r.qty = 10;
            r.type = OrdType::Market;
            if (stage.load() == 0) {
                stage = 1;
                r.side = Side::Buy;
                ctx.submit_order(r);
            } else if (stage.load() == 2) {
                r.side = Side::Sell;          // underwater: hold mode declines it
                if (ctx.submit_order(r) == 0) ++refused;
            }
        }
        void on_fill(IStrategyContext&, const Fill&) noexcept override {
            int expect = 1;
            stage.compare_exchange_strong(expect, 2);
        }
        void on_stop(IStrategyContext&) noexcept override {}
        void destroy() noexcept override {}
    } strat;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 100'000.0;
    cfg.risk.disable_auto_halt = true;   // hold-until-profitable
    REQUIRE(eng.start_live(cfg, {&strat}));
    pump(eng, [&] { return strat.stage.load() >= 2; });   // buys at 50
    // Waits on the PUBLISHED row, not the strategy's own counter: the snapshot
    // lags the engine thread by a publish.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        eng.push_live_tick("AAA", 2, 40.0, 0.0);   // now underwater
        const LiveSnapshot cur = eng.live_snapshot();
        const RefusalStat* held = row(cur, RejectCause::HoldBlocksLoss);
        if (held && held->count >= 5) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();

    const RefusalStat* r = row(s, RejectCause::HoldBlocksLoss);
    REQUIRE(r != nullptr);
    CHECK(r->count >= 5);
    CHECK(r->exit_count == r->count);   // every one of them WAS an exit
    CHECK(r->exit_order);
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(kExitOrderRefusedTag) == std::string::npos);    // never Critical
    CHECK(all.find(kOrderRefusedRepeatTag) == std::string::npos);  // never Warning
    // ...and exactly one Info line, however many times it happens.
    size_t said = 0;
    for (size_t at = all.find(kOrderRefusedTag); at != std::string::npos;
         at = all.find(kOrderRefusedTag, at + 1))
        ++said;
    CHECK(said == 1);
    check_no_blank_reasons(s);
}

namespace {
// A bracket: the parent's id comes back from submit(), the take-profit and
// stop legs get theirs INSIDE the adapter (tws_broker.cpp handle_submit) and are
// never returned, so the engine's blotter has never heard of them.
struct BracketLegBroker : IBrokerAdapter {
    static constexpr uint64_t kLegId = 4242;   // minted inside the adapter
    std::mutex mu;
    std::deque<EngineEvent> q;
    uint64_t next = 1;

    uint64_t submit(const OrderRequest& r, int64_t) override {
        std::lock_guard l(mu);
        const uint64_t id = next++;
        EngineEvent f{};   // fills instantly, so a position exists to protect
        f.type = static_cast<uint16_t>(EvType::Fill);
        f.symbol_id = r.symbol_id;
        f.u.fill.order_id = id;
        f.u.fill.price = 50.0;
        f.u.fill.qty = r.qty;
        f.u.fill.side = static_cast<uint8_t>(r.side);
        q.push_back(f);
        return id;
    }
    // IBKR refuses the take-profit leg. NOT kEvFlagProtective: only the stop leg
    // is tagged (tws_broker.cpp), which is precisely why the engine's other
    // safety net does not cover this one.
    void reject_take_profit(uint32_t sid) {
        std::lock_guard l(mu);
        EngineEvent e{};
        e.type = static_cast<uint16_t>(EvType::OrderCancel);
        e.flags = kEvFlagRejected;
        e.symbol_id = sid;
        e.u.order.order_id = kLegId;
        q.push_back(e);
    }
    bool cancel(uint64_t) override { return true; }
    void cancel_all() override {}
    void flatten() override {}
    bool poll_event(EngineEvent& out) override {
        std::lock_guard l(mu);
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    bool ready() const override { return true; }
    RejectReason take_reject(uint64_t id) override {
        if (id != kLegId) return {};
        return {RejectCause::BrokerRejected, 201,
                "Order rejected - reason:Exchange is closed."};
    }
};
} // namespace

TEST_CASE("refusal: a rejected order the blotter never held is still counted and paged") {
    // The broker-reject path used to require a blotter row before it would touch
    // the refusal table, and two whole classes of rejection do not have one:
    //   * a bracket's take-profit/stop legs, whose ids are minted inside the
    //     adapter and never returned from submit();
    //   * any resting leg whose row aged out of the 200-entry ring on a session
    //     that spans days (this one has).
    // Such a rejection produced NOTHING an operator could see: refusals empty,
    // refusal_count 0, refused_exits 0, reject_count 0 - the tripwire that
    // started this whole investigation - and the only trace was one line rated
    // Warning, indistinguishable from routine order noise.
    BracketLegBroker broker;
    Engine eng;
    OneShotStrat strat;
    strat.req = mkt(Side::Buy, 10.0);
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 100'000.0;
    cfg.broker = &broker;
    REQUIRE(eng.start_live(cfg, {&strat}));
    // Wait for the parent to fill: the position is what makes the refused leg an
    // exit rather than an entry.
    REQUIRE(pump(eng, [&] {
        const LiveSnapshot s = eng.live_snapshot();
        return !s.symbols.empty() && s.symbols[0].position.qty != 0.0;
    }));
    broker.reject_take_profit(1);
    pump(eng, [&] { return has_cause(eng.live_snapshot(), RejectCause::BrokerRejected); });
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();

    const RefusalStat* r = row(s, RejectCause::BrokerRejected);
    REQUIRE(r != nullptr);                 // it is IN the table at all
    CHECK(r->symbol == "AAA");             // and it says which symbol
    CHECK(r->code == 201);
    CHECK(r->message.find("Exchange is closed") != std::string::npos);
    // An unidentifiable rejection on a symbol that HOLDS a position is treated
    // as an exit. Deliberately asymmetric: over-calling it costs one page,
    // under-calling it costs the naked position.
    CHECK(r->exit_order);
    CHECK(r->exit_count == 1);
    int rejected_rows = 0;
    for (const OrderRecord& o : s.orders)
        if (o.status == OrderStatus::Rejected) ++rejected_rows;
    CHECK(rejected_rows >= 1);             // /diag reject_count moves off 0
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(kExitOrderRefusedTag) != std::string::npos);
    check_no_blank_reasons(s);
}

namespace {
// Orders off a BAR and nothing else - the shape of a symbol that gets backfill
// on a feed reconnect but has not ticked this session.
struct BarBuyer : IStrategy {
    std::atomic<bool> sent{false};
    std::atomic<uint64_t> id{0};
    void on_init(IStrategyContext&) noexcept override {}
    void on_bar(IStrategyContext& ctx, uint32_t sid, const Bar&) noexcept override {
        if (sent.exchange(true)) return;
        OrderRequest r{};
        r.symbol_id = sid;
        r.side = Side::Buy;
        r.type = OrdType::Market;
        r.qty = 10;
        id = ctx.submit_order(r);
    }
    void on_tick(IStrategyContext&, uint32_t, const Tick&) noexcept override {}
    void on_fill(IStrategyContext&, const Fill&) noexcept override {}
    void on_stop(IStrategyContext&) noexcept override {}
    void destroy() noexcept override {}
};
} // namespace

TEST_CASE("live: an order off a BACKFILL bar is priced, exactly as replay prices it") {
    // 0.23.0 added the missing set_last_price to run_replay's captured-bar
    // branch and not to run_live's, so the two disagreed about the same event:
    // live priced the symbol at 0 and refused the order price_unavailable, while
    // replaying the capture priced it off the bar close and placed it. A capture
    // that does not reproduce the session it was taken from cannot be used to
    // explain the /diag row it was taken to explain.
    //
    // Not an exotic path: ibkr_feed.cpp and polygon_feed.cpp backfill EVERY
    // configured symbol on each stream reconnect, so a symbol that has never
    // ticked still receives bars.
    BarBuyer strat;
    Engine eng;
    LiveConfig cfg;
    cfg.symbols = {"AAA"};
    cfg.bar_seconds = 100'000;
    cfg.initial_cash = 100'000.0;
    cfg.risk.max_position_notional = 5'000.0;   // arms the clamp that needs a price
    REQUIRE(eng.start_live(cfg, {&strat}));
    EngineEvent bar{};
    bar.type = static_cast<uint16_t>(EvType::Bar);
    bar.symbol_id = 1;
    bar.ts_event_ns = 1;
    bar.u.bar.open = bar.u.bar.high = bar.u.bar.low = bar.u.bar.close = 50.0;
    bar.u.bar.volume = 1000.0;
    // NO tick is ever pushed: the bar close is the only price this symbol has.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!strat.sent.load() && std::chrono::steady_clock::now() < deadline) {
        eng.push_feed_event(bar);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // The submit itself happens on the engine thread inside on_bar; give the
    // refusal (if any) a moment to reach the snapshot.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const LiveSnapshot s = eng.live_snapshot();
    eng.stop_live();
    CHECK(strat.sent.load());
    CHECK(strat.id.load() != 0);   // it was PLACED, sized against the bar close
    // Asserted against the LOG, not the snapshot: with no ticks at all the live
    // loop is idle and may not have published a refusal row yet, so an empty
    // snapshot would pass this for the wrong reason.
    std::string all;
    for (std::string line; eng.pop_log(line);) all += line + "\n";
    INFO("engine log:\n", all);
    CHECK(all.find(reject_cause_slug(RejectCause::PriceUnavailable)) == std::string::npos);
    CHECK(all.find(kOrderRefusedTag) == std::string::npos);
    check_no_blank_reasons(s);
}
