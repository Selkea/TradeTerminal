#pragma once

#include "journal.h"

#include <cstdint>
#include <vector>

namespace tt::ui {

// Trade history: PnL by day, session list, fills of the selected session.
// Re-queries only when the journal's revision changes (writes are rare).
class JournalPanel {
public:
    explicit JournalPanel(TradeJournal& journal) : journal_(journal) {}
    void draw(bool* open);

private:
    void refresh();

    TradeJournal& journal_;
    uint64_t seen_rev_ = 0;
    std::vector<TradeJournal::DayRow> days_;
    std::vector<TradeJournal::SessionRow> sessions_;
    std::vector<TradeJournal::FillRow> fills_;
    int64_t selected_session_ = 0;
};

} // namespace tt::ui
