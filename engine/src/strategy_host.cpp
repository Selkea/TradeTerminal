#include "engine/strategy_host.h"

#include <windows.h>

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace tt {

namespace {
using SdkVersionFn = uint32_t (*)();
using InfoFn = const StrategyInfo* (*)();
using CreateFn = IStrategy* (*)();

std::string key_of(const std::string& src_path) {
    return fs::path(src_path).filename().string();
}
} // namespace

StrategyHost::StrategyHost(std::string gxx_path, std::string sdk_include_dir,
                           std::string out_dir)
    : gxx_(std::move(gxx_path)),
      sdk_include_(std::move(sdk_include_dir)),
      out_dir_(std::move(out_dir)) {
    std::error_code ec;
    fs::create_directories(out_dir_, ec);
}

StrategyHost::~StrategyHost() {
    // App teardown: the engine (destroyed before the host — see app.h member
    // order) has joined its threads, so leftover instances are unreachable.
    for (auto& [inst, mod] : owners_) {
        inst->destroy();
        --mod->refs;
    }
    owners_.clear();
    for (auto& [key, m] : modules_) release(m.get());
    modules_.clear();
    for (auto& m : retired_) release(m.get());
    retired_.clear();
}

bool StrategyHost::compile(const std::string& src_cpp,
                           const std::function<void(std::string)>& on_output,
                           std::string& dll_out) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const std::string stem = fs::path(src_cpp).stem().string();
    const std::string dll =
        (fs::path(out_dir_) / (stem + "." + std::to_string(ms) + ".dll")).string();

    std::string cmd = "\"" + gxx_ + "\" -std=c++20 -O2 -g -shared -Wall -Wextra"
                      " -I\"" + sdk_include_ + "\""
                      " -o \"" + dll + "\" \"" + src_cpp + "\"";
    if (on_output) on_output("$ " + cmd);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return false;
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(pipe_write);
    if (!ok) {
        CloseHandle(pipe_read);
        if (on_output) on_output("failed to start compiler: " + gxx_);
        return false;
    }
    CloseHandle(pi.hThread);

    std::string acc;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(pipe_read, buf, sizeof(buf), &got, nullptr) && got > 0) {
        acc.append(buf, got);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && on_output) on_output(line);
        }
    }
    if (!acc.empty() && on_output) on_output(acc);
    CloseHandle(pipe_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    if (code != 0) {
        if (on_output) on_output("compile failed (exit " + std::to_string(code) + ")");
        return false;
    }
    dll_out = dll;
    if (on_output) on_output("built " + dll);
    return true;
}

bool StrategyHost::load(const std::string& dll_path, const std::string& src_path,
                        std::string& err) {
    HMODULE m = LoadLibraryExA(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        err = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    const auto ver_fn = reinterpret_cast<SdkVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(m, "tt_sdk_version")));
    if (!ver_fn) {
        FreeLibrary(m);
        err = "not a TradeTerminal strategy (missing tt_sdk_version)";
        return false;
    }
    const uint32_t v = ver_fn();
    if (v != TT_SDK_VERSION) {
        FreeLibrary(m);
        err = "SDK version mismatch: dll=" + std::to_string(v) +
              " host=" + std::to_string(TT_SDK_VERSION);
        return false;
    }

    const auto info_fn = reinterpret_cast<InfoFn>(
        reinterpret_cast<void*>(GetProcAddress(m, "tt_strategy_info")));
    const auto create_fn = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(m, "tt_create_strategy")));
    if (!info_fn || !create_fn) {
        FreeLibrary(m);
        err = "missing tt_strategy_info/tt_create_strategy exports";
        return false;
    }
    const StrategyInfo* info = info_fn();
    if (!info) {
        FreeLibrary(m);
        err = "strategy info returned null";
        return false;
    }

    auto mod = std::make_unique<Module>();
    mod->meta.key = key_of(src_path);
    mod->meta.name = info->name ? info->name : fs::path(src_path).stem().string();
    mod->meta.dll_path = dll_path;
    mod->meta.src_path = src_path;
    for (uint32_t i = 0; i < info->param_count; ++i) {
        const ParamDesc& p = info->params[i];
        mod->meta.params.push_back(Param{p.name ? p.name : "", p.def, p.min, p.max});
    }
    mod->hmodule = m;
    mod->create = create_fn;
    std::error_code ec;
    mod->src_mtime = fs::last_write_time(src_path, ec);

    // Replace any module already under this key; running instances keep it
    // alive until they finish.
    auto it = modules_.find(mod->meta.key);
    if (it != modules_.end()) {
        retire(std::move(it->second));
        modules_.erase(it);
    }
    modules_.emplace(mod->meta.key, std::move(mod));
    return true;
}

void StrategyHost::unload(const std::string& key) {
    auto it = modules_.find(key);
    if (it == modules_.end()) return;
    retire(std::move(it->second));
    modules_.erase(it);
}

void StrategyHost::retire(std::unique_ptr<Module> m) {
    if (m->refs == 0) {
        release(m.get());
        return;   // unique_ptr frees the bookkeeping
    }
    m->retired = true;
    retired_.push_back(std::move(m));
}

void StrategyHost::release(Module* m) {
    if (m->hmodule) {
        FreeLibrary(static_cast<HMODULE>(m->hmodule));
        m->hmodule = nullptr;
    }
    std::error_code ec;
    fs::remove(m->meta.dll_path, ec);   // best-effort; ignored if still mapped
}

bool StrategyHost::stale(const std::string& key) const {
    const auto it = modules_.find(key);
    if (it == modules_.end()) return true;
    std::error_code ec;
    const auto now_mtime = fs::last_write_time(it->second->meta.src_path, ec);
    if (ec) return false;   // source vanished: the loaded module is all we have
    return now_mtime > it->second->src_mtime;
}

bool StrategyHost::info(const std::string& key, ModuleView& out) const {
    const auto it = modules_.find(key);
    if (it == modules_.end()) return false;
    out = it->second->meta;
    out.instances = it->second->refs;
    return true;
}

std::vector<StrategyHost::ModuleView> StrategyHost::modules() const {
    std::vector<ModuleView> out;
    out.reserve(modules_.size());
    for (const auto& [key, m] : modules_) {
        out.push_back(m->meta);
        out.back().instances = m->refs;
    }
    return out;
}

IStrategy* StrategyHost::create_instance(const std::string& key) {
    const auto it = modules_.find(key);
    if (it == modules_.end()) return nullptr;
    IStrategy* inst = it->second->create();
    if (!inst) return nullptr;
    ++it->second->refs;
    owners_[inst] = it->second.get();
    return inst;
}

void StrategyHost::destroy_instance(IStrategy* inst) {
    const auto it = owners_.find(inst);
    if (it == owners_.end()) return;   // not ours (or double-destroy): ignore
    Module* mod = it->second;
    owners_.erase(it);
    inst->destroy();
    --mod->refs;
    if (mod->retired && mod->refs == 0) {
        for (auto r = retired_.begin(); r != retired_.end(); ++r) {
            if (r->get() == mod) {
                release(mod);
                retired_.erase(r);
                break;
            }
        }
    }
}

void StrategyHost::sweep_stale() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(out_dir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".dll") continue;
        const std::string p = entry.path().string();
        bool in_use = false;
        for (const auto& [key, m] : modules_)
            if (m->meta.dll_path == p) in_use = true;
        for (const auto& m : retired_)
            if (m->meta.dll_path == p) in_use = true;
        if (in_use) continue;
        std::error_code del_ec;
        fs::remove(entry.path(), del_ec);   // locked files just stay
    }
}

} // namespace tt
