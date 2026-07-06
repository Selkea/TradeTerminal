#include "account_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace tt::ui {

namespace {

const wchar_t* kDpapiLabel = L"TradeTerminal credentials";

std::string to_hex(const unsigned char* p, size_t n) {
    static constexpr char d[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xf]);
    }
    return s;
}

bool from_hex(const std::string& s, std::vector<unsigned char>& out) {
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<unsigned char>(hi << 4 | lo));
    }
    return true;
}

// "" on failure — an empty credential is never valid anyway.
std::string protect(const std::string& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, kDpapiLabel, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string hex = to_hex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return hex;
}

std::string unprotect(const std::string& hex) {
    std::vector<unsigned char> blob;
    if (!from_hex(hex, blob) || blob.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace

std::string AccountStore::active_name(const std::string& provider) const {
    for (const auto& [p, n] : active_)
        if (p == provider) return n;
    return {};
}

std::optional<Account> AccountStore::active(const std::string& provider) const {
    const std::string n = active_name(provider);
    return n.empty() ? std::nullopt : get(n);
}

std::optional<Account> AccountStore::get(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name != name) continue;
        Account a;
        a.name = name;
        a.provider = entries_[i].provider;
        a.key_id = unprotect(enc_[i].key_hex);
        // Secret is legitimately empty for single-key providers (Polygon).
        a.secret = enc_[i].secret_hex.empty() ? "" : unprotect(enc_[i].secret_hex);
        if (a.key_id.empty()) return std::nullopt;   // DPAPI refused / corrupt
        return a;
    }
    return std::nullopt;
}

void AccountStore::upsert(const Account& a, bool make_active) {
    Enc e;
    e.key_hex = protect(a.key_id);
    e.secret_hex = a.secret.empty() ? "" : protect(a.secret);
    if (e.key_hex.empty()) return;   // DPAPI failed
    bool found = false;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == a.name) {
            entries_[i].provider = a.provider;
            enc_[i] = std::move(e);
            found = true;
            break;
        }
    if (!found) {
        entries_.push_back({a.name, a.provider});
        enc_.push_back(std::move(e));
    }
    if (make_active) {
        bool had = false;
        for (auto& [p, n] : active_)
            if (p == a.provider) {
                n = a.name;
                had = true;
            }
        if (!had) active_.emplace_back(a.provider, a.name);
    }
    save();
}

bool AccountStore::set_active(const std::string& name) {
    for (const Entry& e : entries_)
        if (e.name == name) {
            bool had = false;
            for (auto& [p, n] : active_)
                if (p == e.provider) {
                    n = name;
                    had = true;
                }
            if (!had) active_.emplace_back(e.provider, name);
            save();
            return true;
        }
    return false;
}

void AccountStore::sign_out(const std::string& provider) {
    for (auto it = active_.begin(); it != active_.end(); ++it)
        if (it->first == provider) {
            active_.erase(it);
            break;
        }
    save();
}

void AccountStore::remove(const std::string& name) {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name) {
            const std::string provider = entries_[i].provider;
            entries_.erase(entries_.begin() + i);
            enc_.erase(enc_.begin() + i);
            if (active_name(provider) == name) sign_out(provider);
            break;
        }
    save();
}

void AccountStore::load() {
    std::ifstream f(path_);
    if (!f) return;
    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return;
    for (const auto& a : j.value("accounts", nlohmann::json::array())) {
        if (!a.is_object()) continue;
        const std::string name = a.value("name", "");
        if (name.empty()) continue;
        entries_.push_back({name, a.value("provider", "alpaca")});
        enc_.push_back({a.value("key_id", ""), a.value("secret", "")});
    }
    const auto act = j.find("active");
    if (act != j.end() && act->is_object()) {
        for (const auto& [prov, name] : act->items())
            if (name.is_string()) active_.emplace_back(prov, name.get<std::string>());
    } else if (act != j.end() && act->is_string() && !act->get<std::string>().empty()) {
        // Pre-provider format: a single active account, implicitly Alpaca.
        active_.emplace_back("alpaca", act->get<std::string>());
    }
    // Drop dangling active references.
    for (auto it = active_.begin(); it != active_.end();) {
        bool exists = false;
        for (const Entry& e : entries_)
            if (e.name == it->second) exists = true;
        it = exists ? it + 1 : active_.erase(it);
    }
}

void AccountStore::save() const {
    nlohmann::json accounts = nlohmann::json::array();
    for (size_t i = 0; i < entries_.size(); ++i)
        accounts.push_back({{"name", entries_[i].name},
                            {"provider", entries_[i].provider},
                            {"key_id", enc_[i].key_hex},
                            {"secret", enc_[i].secret_hex}});
    nlohmann::json act = nlohmann::json::object();
    for (const auto& [p, n] : active_) act[p] = n;
    const nlohmann::json j{{"active", act}, {"accounts", accounts}};
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::trunc);
    if (f) f << j.dump(2);
}

} // namespace tt::ui
