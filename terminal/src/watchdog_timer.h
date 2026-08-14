#pragma once

// The since/threshold/re-alert timer the state-based watchdogs share.
//
// pump_broker_watchdog and pump_orphan_watchdog were the same eleven lines twice:
// note when a bad state first appeared, stay quiet through a grace period so a
// transient blip doesn't page, page once, re-page on a slow cadence while it
// persists, and say so when it clears. Extracted so the TIMER can be tested
// without an App, an ImGui context or a broker — which is the part that was
// broken.
//
// WHAT WAS BROKEN. Both watchdogs ran only from App::draw(), on ImGui::GetTime().
// That clock only advances inside ImGui_ImplGlfw_NewFrame, and main.cpp skipped
// the whole frame while the window was iconified — so on 2026-08-11 a minimized
// window did not merely delay these alerts, it stopped their clock dead: an
// outage could run for hours with `now - since_s` pinned at whatever it read
// when the window went down. Worse, the first restored frame jumped
// ImGui::GetTime() forward by the entire minimized duration in one step, so the
// episode would then page with a down-time it had never actually measured.
//
// Hence the contract below: now_s MUST come from a steady clock that advances
// whether or not a frame is drawn (App::mono_s, tick_clock.h). Everything else
// here is pure arithmetic on that number, which is what makes it testable.

namespace tt::ui {

class WatchdogTimer {
public:
    enum class Action {
        None,        // nothing to say
        Page,        // the bad state has outlasted its grace (or its re-alert gap)
        Recovered,   // it cleared, and we had paged about it
    };

    // One observation. `bad` is the watched condition right now; `now_s` is
    // steady seconds (see the header note — NOT ImGui::GetTime()); `alert_after_s`
    // is how long a bad state must persist before the first page; `re_alert_s` is
    // the gap before the FIRST repeat page while it persists.
    //
    // `backoff_mult` widens that gap on each repeat, capped at `re_alert_cap_s`
    // (0 = uncapped). The default of 1.0 is a flat repeat, which is what every
    // caller had before and what a short, self-clearing outage still wants.
    //
    // WHY A BACKOFF EXISTS AT ALL. On 2026-08-14 a single unchanged book
    // divergence — NVDA app=0 broker=20, stable for over eight hours — paged
    // Critical every 15 minutes for the whole session, ~32 times, alongside a
    // half-hourly history-stale page. The condition was real and worth exactly
    // one page; what it produced was a channel the operator stops reading, which
    // is strictly worse than not paging at all because the NEXT alert is the one
    // that matters. A repeat page carries no information the first did not: the
    // alert text already quotes how long the condition has run.
    //
    // The shape (immediately, then the gap, then x3, capped at an hour) is the
    // one engine/reject.h already argued for refused exits, kept identical so
    // there is one repeat policy in this codebase rather than two. Bounded on
    // both sides on purpose: never silent, because a divergence nobody is
    // looking at is how a real position ends up unmanaged, and never a flood.
    Action update(bool bad, double now_s, double alert_after_s, double re_alert_s,
                  double backoff_mult = 1.0, double re_alert_cap_s = 0.0) {
        if (!bad) {
            // Only announce a recovery to whoever was told about the outage. A
            // blip that cleared inside the grace period was never worth a page,
            // so its "all clear" is not worth one either.
            const bool announce = alerted_;
            reset();
            return announce ? Action::Recovered : Action::None;
        }
        if (!in_episode_) {   // start of a bad episode
            in_episode_ = true;
            since_s_ = now_s;
        }
        if (now_s - since_s_ < alert_after_s) return Action::None;
        if (alerted_ && now_s - last_alert_s_ < re_alert_cur_s_) return Action::None;
        // Widen only on a REPEAT. The first page of an episode always uses the
        // caller's base gap, so an episode that clears and returns starts loud
        // again rather than inheriting the backoff of the one before it.
        if (alerted_ && backoff_mult > 1.0) {
            re_alert_cur_s_ *= backoff_mult;
            if (re_alert_cap_s > 0.0 && re_alert_cur_s_ > re_alert_cap_s)
                re_alert_cur_s_ = re_alert_cap_s;
        } else {
            re_alert_cur_s_ = re_alert_s;
        }
        alerted_ = true;
        last_alert_s_ = now_s;
        return Action::Page;
    }

    // Stand down without announcing anything: the watchdog no longer applies at
    // all (no live session, no broker). Distinct from update(false, ...) on
    // purpose — "the session ended" is not "the outage cleared", and a Warning
    // saying the broker reconnected every time a session stops would train the
    // operator to ignore the channel.
    void reset() {
        in_episode_ = false;
        alerted_ = false;
        since_s_ = 0.0;
        last_alert_s_ = 0.0;
        re_alert_cur_s_ = 0.0;   // the next episode pages on its own base gap
    }

    // How long the current bad episode has run (0 if there isn't one). The page
    // quotes this, so it has to be readable — and reading it AFTER a Page is
    // correct, because a Page does not clear the episode.
    double age_s(double now_s) const { return in_episode_ ? now_s - since_s_ : 0.0; }
    bool in_episode() const { return in_episode_; }
    bool alerted() const { return alerted_; }

private:
    // Flags rather than a 0.0 sentinel on the timestamps. The clock these run on
    // is mono_s(), measured from App construction, so t=0.0 is a REAL time — an
    // "is it zero?" test would read the first instants of the process as "no
    // episode in progress" and could never page about them.
    bool in_episode_ = false;
    bool alerted_ = false;
    double since_s_ = 0.0;
    double last_alert_s_ = 0.0;
    // The gap currently in force, which is the base gap until a repeat widens it.
    double re_alert_cur_s_ = 0.0;
};

} // namespace tt::ui
