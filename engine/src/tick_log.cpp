#include "engine/tick_log.h"

#include <chrono>
#include <cstring>

namespace tt {

namespace {
constexpr char kMagic[4] = {'T', 'T', 'K', '1'};
constexpr uint32_t kVersion = 1;
} // namespace

bool TickLogWriter::open(const std::string& path, const std::vector<std::string>& symbols,
                         int bar_seconds) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    // Writer-thread side: big stdio buffer so the OS write happens in ~1 MiB
    // chunks regardless of tick cadence.
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    written_ = dropped_ = 0;

    const uint32_t version = kVersion;
    const uint32_t bar_s = static_cast<uint32_t>(bar_seconds);
    const uint32_t n_sym = static_cast<uint32_t>(symbols.size());
    bool ok = std::fwrite(kMagic, 4, 1, f_) == 1 &&
              std::fwrite(&version, 4, 1, f_) == 1 &&
              std::fwrite(&bar_s, 4, 1, f_) == 1 &&
              std::fwrite(&n_sym, 4, 1, f_) == 1;
    for (const std::string& s : symbols) {
        const uint16_t len = static_cast<uint16_t>(s.size());
        ok = ok && std::fwrite(&len, 2, 1, f_) == 1 &&
             (len == 0 || std::fwrite(s.data(), len, 1, f_) == 1);
    }
    if (!ok) {
        std::fclose(f_);
        f_ = nullptr;
        return false;
    }

    if (!ring_) ring_ = std::make_unique<Ring>();
    EngineEvent drainleft;
    while (ring_->try_pop(drainleft)) {}   // stale events from a failed run
    stop_.store(false, std::memory_order_relaxed);
    writer_ = std::thread([this] { drain_loop(); });
    open_ = true;
    return true;
}

void TickLogWriter::drain_loop() {
    EngineEvent ev;
    for (;;) {
        bool worked = false;
        while (ring_->try_pop(ev)) {
            std::fwrite(&ev, sizeof ev, 1, f_);
            worked = true;
        }
        if (!worked) {
            if (stop_.load(std::memory_order_acquire)) {
                // stop_ is set after the producer's last write(), so one
                // final drain pass cannot miss anything.
                while (ring_->try_pop(ev)) std::fwrite(&ev, sizeof ev, 1, f_);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void TickLogWriter::close() {
    if (!open_ && !writer_.joinable()) return;
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();   // drains the ring first
    if (f_) std::fclose(f_);
    f_ = nullptr;
    open_ = false;
}

bool tick_log_read(const std::string& path, TickLog& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    char magic[4];
    uint32_t version = 0, bar_s = 0, n_sym = 0;
    if (std::fread(magic, 4, 1, f) != 1 || std::memcmp(magic, kMagic, 4) != 0 ||
        std::fread(&version, 4, 1, f) != 1 || version != kVersion ||
        std::fread(&bar_s, 4, 1, f) != 1 || std::fread(&n_sym, 4, 1, f) != 1 ||
        n_sym == 0 || n_sym > 256) {
        err = "not a TTK1 tick log";
        std::fclose(f);
        return false;
    }
    out.symbols.clear();
    out.bar_seconds = static_cast<int>(bar_s);
    for (uint32_t i = 0; i < n_sym; ++i) {
        uint16_t len = 0;
        char buf[512];
        if (std::fread(&len, 2, 1, f) != 1 || len > sizeof buf ||
            (len > 0 && std::fread(buf, len, 1, f) != 1)) {
            err = "corrupt symbol table";
            std::fclose(f);
            return false;
        }
        out.symbols.emplace_back(buf, len);
    }
    out.events.clear();
    EngineEvent ev;
    while (std::fread(&ev, sizeof ev, 1, f) == 1) out.events.push_back(ev);
    std::fclose(f);
    if (out.events.empty()) {
        err = "no events in log";
        return false;
    }
    return true;
}

} // namespace tt
