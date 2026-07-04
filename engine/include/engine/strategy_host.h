#pragma once
// Compiles strategy .cpp files with g++ and hot-loads the resulting DLLs.
//
// Windows locks a loaded DLL's file, so each build gets a fresh versioned
// name (<stem>.<millis>.dll) in the output dir — g++ never has to overwrite
// a file that's mapped. Old versions are swept (best-effort) at startup and
// after unload. Load order: tt_sdk_version is checked before anything else;
// a mismatched DLL is refused, never called.

#include "tt/strategy_api.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tt {

class StrategyHost {
public:
    struct Loaded {
        IStrategy* instance = nullptr;   // DLL-owned; freed via destroy()
        std::string name;                // display name from StrategyInfo
        std::string dll_path;
        std::string src_path;
        struct Param { std::string name; double def, min, max; };
        std::vector<Param> params;       // copied out of DLL memory at load
    };

    StrategyHost(std::string gxx_path, std::string sdk_include_dir,
                 std::string out_dir);
    ~StrategyHost();

    // Blocking; run it off the UI thread. Streams compiler output lines to
    // on_output. On success dll_out receives the fresh DLL path.
    bool compile(const std::string& src_cpp,
                 const std::function<void(std::string)>& on_output,
                 std::string& dll_out);

    // Refuses SDK version mismatches and missing exports. Any previously
    // loaded strategy is unloaded first (caller must ensure the engine is
    // idle — enforce with Engine::running()).
    bool load(const std::string& dll_path, const std::string& src_path,
              std::string& err);
    void unload();

    const Loaded* current() const { return loaded_.instance ? &loaded_ : nullptr; }

    // Bumped on every load/unload; lets queued work detect strategy swaps.
    uint64_t generation() const { return generation_; }

    // Delete stale versioned DLLs in out_dir (skips anything still locked).
    void sweep_stale();

    const std::string& gxx_path() const { return gxx_; }

private:
    std::string gxx_, sdk_include_, out_dir_;
    Loaded loaded_;
    void* module_ = nullptr;   // HMODULE
    uint64_t generation_ = 0;
};

} // namespace tt
