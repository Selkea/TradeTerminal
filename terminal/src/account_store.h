#pragma once
// Saved Alpaca accounts. Credentials never touch disk in plaintext: key id
// and secret are encrypted with Windows DPAPI (CryptProtectData), bound to
// the current Windows user + machine, and hex-encoded into accounts.json.
// Decryption happens on demand, so plaintext secrets are not kept resident.
//
// UI-thread only; every mutation persists immediately.

#include <optional>
#include <string>
#include <vector>

namespace tt::ui {

struct Account {
    std::string name;     // user-chosen label, unique
    std::string key_id;   // APCA-API-KEY-ID
    std::string secret;   // APCA-API-SECRET-KEY
};

class AccountStore {
public:
    explicit AccountStore(std::string path) : path_(std::move(path)) { load(); }

    const std::vector<std::string>& names() const { return names_; }
    const std::string& active_name() const { return active_; }
    bool signed_in() const { return !active_.empty(); }

    // Decrypts on demand. nullopt if missing or DPAPI refuses (e.g. the file
    // was copied from another Windows user/machine).
    std::optional<Account> active() const { return get(active_); }
    std::optional<Account> get(const std::string& name) const;

    void upsert(const Account& a, bool make_active);
    bool set_active(const std::string& name);
    void sign_out();                          // keeps the saved account
    void remove(const std::string& name);

private:
    void load();
    void save() const;

    std::string path_;
    std::string active_;
    std::vector<std::string> names_;
    struct Enc {
        std::string key_hex, secret_hex;      // DPAPI blobs, hex-encoded
    };
    std::vector<Enc> enc_;                    // parallel to names_
};

} // namespace tt::ui
