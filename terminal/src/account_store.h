#pragma once
// Saved provider credentials (Alpaca key+secret, Polygon API key, ...).
// Nothing touches disk in plaintext: fields are encrypted with Windows DPAPI
// (CryptProtectData), bound to the current Windows user + machine, and
// hex-encoded into accounts.json. Decryption happens on demand.
//
// One account can be active PER PROVIDER (an Alpaca login and a Polygon key
// are used simultaneously). IBKR deliberately has no entry here — its login
// lives in the Client Portal Gateway's own browser session.
//
// UI-thread only; every mutation persists immediately.

#include <optional>
#include <string>
#include <vector>

namespace tt::ui {

struct Account {
    std::string name;                  // user-chosen label, unique across providers
    std::string provider = "alpaca";   // "alpaca" | "polygon"
    std::string key_id;                // Alpaca key id / Polygon API key
    std::string secret;                // Alpaca secret; empty for Polygon
};

class AccountStore {
public:
    explicit AccountStore(std::string path) : path_(std::move(path)) { load(); }

    struct Entry {
        std::string name, provider;
    };
    const std::vector<Entry>& list() const { return entries_; }

    std::string active_name(const std::string& provider) const;
    bool signed_in(const std::string& provider) const {
        return !active_name(provider).empty();
    }
    // Decrypts on demand. nullopt if missing or DPAPI refuses (e.g. the file
    // was copied from another Windows user/machine).
    std::optional<Account> active(const std::string& provider) const;
    std::optional<Account> get(const std::string& name) const;

    void upsert(const Account& a, bool make_active);
    bool set_active(const std::string& name);        // provider inferred
    void sign_out(const std::string& provider);      // keeps the saved account
    void remove(const std::string& name);

private:
    void load();
    void save() const;

    std::string path_;
    std::vector<Entry> entries_;
    struct Enc {
        std::string key_hex, secret_hex;   // DPAPI blobs, hex-encoded
    };
    std::vector<Enc> enc_;                          // parallel to entries_
    std::vector<std::pair<std::string, std::string>> active_;   // provider -> name
};

} // namespace tt::ui
