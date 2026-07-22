#pragma once
// Registry for statically-linked ("promoted") strategies — TT_STRATEGY .cpp
// files compiled directly into tt_terminal instead of hot-loaded as a DLL.
// Populated entirely by static initializers before main() runs (see the
// TT_STRATEGY_STATIC_LINK branch of TT_STRATEGY in strategy_api.h). Read-only
// after that: safe from any thread, no locking needed.

#include "tt/strategy_api.h"

#include <string>
#include <vector>

namespace tt {

struct StaticStrategyEntry {
    const char* key;   // matches the DLL-key convention, e.g. "orb_breakout.cpp"
    const StrategyInfo* info;
    IStrategy* (*create)();
};

// Meyer's singleton: the vector is constructed on first call, not at global
// static-init time, so it doesn't matter which TU's global constructors run
// first — every TU (registrars included) sees the same instance already
// built by the time it touches it.
inline std::vector<StaticStrategyEntry>& static_strategy_registry() {
    static std::vector<StaticStrategyEntry> reg;
    return reg;
}

inline const StaticStrategyEntry* find_static_strategy(const std::string& key) {
    for (const auto& e : static_strategy_registry())
        if (key == e.key) return &e;
    return nullptr;
}

namespace detail {
inline bool register_static_strategy(const char* key, const StrategyInfo* info,
                                      IStrategy* (*create)()) {
    static_strategy_registry().push_back({key, info, create});
    return true;
}
} // namespace detail

} // namespace tt
