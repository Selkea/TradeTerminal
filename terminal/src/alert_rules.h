#pragma once
// Push-alert classification for a log line, kept as a pure function separate
// from AlertNotifier's beep/webhook side effects so the rules are unit-testable
// and there's ONE place that decides what pages the phone.

#include <string>

namespace tt::ui {

enum class AlertClass { None, Info, Warning, Critical };

inline AlertClass classify_alert(const std::string& l) {
    auto has = [&](const char* p) { return l.find(p) != std::string::npos; };
    if (has("KILL SWITCH") || has("RISK HALT") || has("WATCHDOG") ||
        has("PROTECTIVE STOP REJECTED"))
        return AlertClass::Critical;
    // NB: match "half-open ORDER" (a broker order submitted but never acked),
    // NOT bare "half-open". The routine data-feed reconnect logs "...data
    // session (half-open)" — it self-heals on every nightly gateway restart and
    // flaps all weekend, and was spamming the phone with Warning pages.
    if (has("rejected") || has("stream lost") || has("auth failed") ||
        has("(drops!)") || has("half-open order"))
        return AlertClass::Warning;
    if (has("live: fill"))
        return AlertClass::Info;
    return AlertClass::None;
}

} // namespace tt::ui
