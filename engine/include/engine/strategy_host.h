#pragma once
// Compiles strategy .cpp files with g++ and hot-loads the resulting DLLs.
//
// Windows locks a loaded DLL's file, so each build gets a fresh versioned
// name (<stem>.<millis>.dll) in the output dir — g++ never has to overwrite
// a file that's mapped. Old versions are swept (best-effort) at startup and
// when modules are released. Load order: tt_sdk_version is checked before
// anything else; a mismatched DLL is refused, never called.
//
// Multiple modules stay loaded side by side, keyed by source basename, and
// every run (backtest, sweep, live, replay) gets its OWN instance from the
// module's factory via create_instance(). Modules are refcounted by their
// outstanding instances: unloading or replacing a module merely retires it,
// and the DLL is freed only when its last instance is destroyed — so a live
// session keeps trading version N while version N+1 loads beside it, and a
// backtest can run strategy B while strategy A trades live.
//
// Threading: compile() only touches the filesystem and may run on a worker
// thread; everything else must be called from a single thread (the UI
// thread). Engine threads never touch the host — they only call the
// IStrategy* they were handed, and the owner of that instance keeps it alive
// until the run's thread is done with it.

#include "tt/strategy_api.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tt {

class StrategyHost {
public:
    struct Param {
        std::string name;
        double def, min, max;
    };
    struct ModuleView {
        std::string key;                // source basename ("sma_crossover.cpp")
        std::string name;               // display name from StrategyInfo
        std::string dll_path, src_path;
        std::vector<Param> params;
        int instances = 0;              // outstanding create_instance() handles
    };

    StrategyHost(std::string gxx_path, std::string sdk_include_dir,
                 std::string out_dir);
    ~StrategyHost();

    // Blocking; run it off the UI thread. Streams compiler output lines to
    // on_output. On success dll_out receives the fresh DLL path.
    bool compile(const std::string& src_cpp,
                 const std::function<void(std::string)>& on_output,
                 std::string& dll_out);

    // Loads dll_path as the module for src_path's basename. An existing
    // module under that key is retired: its instances keep working and its
    // DLL is freed when the last one is destroyed.
    bool load(const std::string& dll_path, const std::string& src_path,
              std::string& err);
    // Retires the module (same deferred-free semantics as replacement).
    void unload(const std::string& key);

    bool has(const std::string& key) const { return modules_.count(key) != 0; }
    // Source file modified since this module was built?
    bool stale(const std::string& key) const;
    bool info(const std::string& key, ModuleView& out) const;
    std::vector<ModuleView> modules() const;   // for the Strategy panel list

    // Fresh instance from the module's factory; nullptr if key is absent.
    IStrategy* create_instance(const std::string& key);
    // Destroys a create_instance() handle. Must only be called once no
    // engine thread can touch the instance anymore. Frees retired modules
    // when their count reaches zero.
    void destroy_instance(IStrategy* inst);

    // Delete stale versioned DLLs in out_dir (skips loaded/locked files).
    void sweep_stale();

    const std::string& gxx_path() const { return gxx_; }

private:
    struct Module {
        ModuleView meta;
        void* hmodule = nullptr;
        IStrategy* (*create)() = nullptr;
        int refs = 0;
        bool retired = false;
        std::filesystem::file_time_type src_mtime{};
    };

    void retire(std::unique_ptr<Module> m);
    void release(Module* m);   // FreeLibrary + best-effort dll delete

    std::string gxx_, sdk_include_, out_dir_;
    std::map<std::string, std::unique_ptr<Module>> modules_;   // by key
    std::vector<std::unique_ptr<Module>> retired_;             // await refs==0
    std::map<IStrategy*, Module*> owners_;
};

} // namespace tt
