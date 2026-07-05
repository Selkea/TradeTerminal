#pragma once
// Session tick capture. The live engine thread appends every market-data
// event it consumes to a .ttk file; the replay path feeds them back through
// ExecSim for a deterministic re-run of the session — post-mortem a bad
// trade, or test a strategy fix against the exact ticks that hurt it.
//
// The engine thread never touches the disk: write() is an SPSC ring push
// (~10 ns, no syscall, no allocation); a dedicated writer thread drains the
// ring and does all file I/O. If the disk stalls long enough to fill the
// ring (16k events), ticks are dropped from the *capture* and counted —
// never delaying the trading loop.
//
// Format (little-endian):  "TTK1" | u32 version | u32 bar_seconds |
//   u32 n_symbols | { u16 len, bytes }*n | EngineEvent[] until EOF.

#include "engine/events.h"
#include "engine/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tt {

struct TickLog {
    std::vector<std::string> symbols;   // session table: id = index + 1
    int bar_seconds = 60;
    std::vector<EngineEvent> events;
};

// open/write/close are engine-thread only; the internal thread owns the FILE.
class TickLogWriter {
public:
    ~TickLogWriter() { close(); }

    bool open(const std::string& path, const std::vector<std::string>& symbols,
              int bar_seconds);
    void write(const EngineEvent& ev) {
        if (!open_) return;
        if (ring_->try_push(ev)) ++written_;
        else ++dropped_;
    }
    void close();   // drains the ring, joins the writer, closes the file

    bool is_open() const { return open_; }
    uint64_t written() const { return written_; }
    uint64_t dropped() const { return dropped_; }

private:
    void drain_loop();

    using Ring = SpscRing<EngineEvent, 1 << 14>;   // 1 MiB, heap-allocated
    std::unique_ptr<Ring> ring_;
    FILE* f_ = nullptr;
    std::thread writer_;
    std::atomic<bool> stop_{false};
    bool open_ = false;
    uint64_t written_ = 0, dropped_ = 0;
};

bool tick_log_read(const std::string& path, TickLog& out, std::string& err);

} // namespace tt
