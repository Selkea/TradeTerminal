#pragma once
// A WRITE-ONLY archive of every historical bar series the app is handed, so a
// parameter study can be run offline against the same data the tournament
// scored on.
//
// WHY. net/bar_cache.h is memory-only and deliberately short-lived — it exists
// to stop one lineup build refetching the same series, nothing more. So six
// months of 5-minute bars live in RAM and evaporate with the process. On
// 2026-09-28 that made a question unanswerable: MUU had been fitted to
// tp_r = 0.5 (a losing reward:risk), the floor was raised to 1.0 on arithmetic
// alone, and where it ACTUALLY belongs needed a sweep over real bars. There were
// none. The session tick captures had been off since July, journal.db carries
// fills with no strategy attribution, and refetching six months meant a TWS data
// connection the live app was holding — not worth risking the gateway for a
// parameter study.
//
// THE APP NEVER READS THIS BACK. That is the whole design.
//
// It would be easy to turn a persisted cache into a read path and save some IB
// requests. It would also be the worst bug this file could have: serving
// yesterday's bars as today's, silently, to a tournament that would then fit and
// trade on them. net/bar_cache.h says "short-lived by design" and it means it.
// This is an archive for humans and offline harnesses, write-only, and the
// freshness guarantees of the live path are untouched.

#include "market_data.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tt::ui {

// Filename for one (symbol, interval) series. Flat, one file per series, so a
// re-delivery REPLACES rather than appends: every write is idempotent, no dedup
// or merge logic exists to get wrong, and disk is bounded by how many distinct
// series have been seen rather than by how long the process has run.
//
// Symbol and interval both come from the feed, so they are sanitised: anything
// that is not alphanumeric, '-', '_' or '.' becomes '_'. A symbol with a path
// separator in it must not be able to write outside the archive directory.
std::string bar_archive_name(const std::string& symbol, const std::string& interval);

// Serialise a series to CSV. Header row, then one row per bar:
//
//     ts,open,high,low,close,volume
//     1787751510,35.07,35.2,35.01,35.12,1000000
//
// ts is epoch SECONDS (Candle::ts's own unit — the engine converts to ns at
// ingest, and writing what the struct actually holds keeps the file honest).
// Doubles use %.10g: exact for prices quoted in cents, and far more compact than
// a fixed 6 decimal places on a volume in the millions.
std::string bar_series_csv(const std::vector<Candle>& candles);

// Did the bytes actually reach the disk? A named predicate for one boolean
// expression, because the expression is the difference between publishing a
// complete archive and publishing a truncated one — and inline inside
// bar_archive_write there was nowhere for a test to reach it. A short write is
// not reproducible portably (it needs a full disk), so the decision is extracted
// and pinned here instead of being left uncovered.
//
// closed_ok is fclose's return, which is where a full disk surfaces: fwrite can
// succeed into a buffer that only fails on the way out.
inline bool archive_write_ok(size_t got, size_t want, bool closed_ok) {
    return got == want && closed_ok;
}

// Write `csv` to `dir/name` ATOMICALLY: to a .tmp sibling, then rename over the
// target. A torn file is worse than a missing one here — a harness reading a
// half-written series gets a plausible answer from truncated data, which is the
// silent-wrong-answer class this project keeps paying for. Returns false on any
// failure, having left any existing file intact.
bool bar_archive_write(const std::string& dir, const std::string& name,
                       const std::string& csv);

// Delete archive files not modified within `days`. Symbols churn daily, so
// without this the directory grows for the life of the box — and a full disk
// stops trading. Returns how many were removed.
//
// Scoped hard: only files directly in `dir` whose name ends in ".csv", never
// recursive, never anything else. `days <= 0` removes nothing.
//
// Takes no clock: the age comes from the file's own last-write time, and a test
// backdates that directly. An earlier cut of this accepted a `now_sec` that
// algebraically cancelled out of the comparison — a parameter that looked like a
// test seam and could not affect the result.
int bar_archive_prune(const std::string& dir, int days);

} // namespace tt::ui
