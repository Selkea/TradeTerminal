#include "account_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace tt::ui {

namespace {

const wchar_t* kDpapiLabel = L"TradeTerminal Alpaca credentials";

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

std::optional<Account> AccountStore::get(const std::string& name) const {
    for (size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] != name) continue;
        Account a;
        a.name = name;
        a.key_id = unprotect(enc_[i].key_hex);
        a.secret = unprotect(enc_[i].secret_hex);
        if (a.key_id.empty() || a.secret.empty()) return std::nullopt;
        return a;
    }
    return std::nullopt;
}

void AccountStore::upsert(const Account& a, bool make_active) {
    Enc e{protect(a.key_id), protect(a.secret)};
    if (e.key_hex.empty() || e.secret_hex.empty()) return;   // DPAPI failed
    bool found = false;
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == a.name) {
            enc_[i] = std::move(e);
            found = true;
            break;
        }
    if (!found) {
        names_.push_back(a.name);
        enc_.push_back(std::move(e));
    }
    if (make_active) active_ = a.name;
    save();
}

bool AccountStore::set_active(const std::string& name) {
    for (const std::string& n : names_)
        if (n == name) {
            active_ = name;
            save();
            return true;
        }
    return false;
}

void AccountStore::sign_out() {
    active_.clear();
    save();
}

void AccountStore::remove(const std::string& name) {
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == name) {
            names_.erase(names_.begin() + i);
            enc_.erase(enc_.begin() + i);
            break;
        }
    if (active_ == name) active_.clear();
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
        names_.push_back(name);
        enc_.push_back({a.value("key_id", ""), a.value("secret", "")});
    }
    const std::string active = j.value("active", "");
    for (const std::string& n : names_)
        if (n == active) active_ = active;
}

void AccountStore::save() const {
    nlohmann::json accounts = nlohmann::json::array();
    for (size_t i = 0; i < names_.size(); ++i)
        accounts.push_back({{"name", names_[i]},
                            {"key_id", enc_[i].key_hex},
                            {"secret", enc_[i].secret_hex}});
    const nlohmann::json j{{"active", active_}, {"accounts", accounts}};
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::trunc);
    if (f) f << j.dump(2);
}

} // namespace tt::ui
