// Connection profiles and app settings, persisted as JSON in %APPDATA%\DbPro.
// Passwords are NOT part of this file: they live in Windows Credential Manager under the profile id.
#pragma once

#include <string>
#include <vector>

namespace dbp {

enum class Env { Development = 0, Testing = 1, Staging = 2, Production = 3 };
const char *env_name(Env e);

struct Profile {
    std::string id;
    std::string name;
    std::string engine; // "sqlite", "postgres", "mysql", "mariadb", "mssql", "mongodb"
    std::string host;
    int port = 0;
    std::string database;
    std::string user;
    std::string path; // SQLite file
    int env = 0;
    bool read_only = false;
    bool tls = true;
    bool trust_server_cert = false; // explicit opt-in, never default
};

struct Settings {
    bool dark = true;
    std::string lang = "en";
    int page_size = 100;
    std::string last_export_dir;
};

class Store {
public:
    // dir empty = the real per-user data directory (tests pass a temp dir).
    explicit Store(std::string dir = {});

    std::vector<Profile> profiles;
    Settings settings;

    void load();
    bool save() const;

    static std::string new_id();
    const Profile *find(const std::string &id) const;
    void upsert(const Profile &p);
    void remove(const std::string &id);

    // Exported/imported profile lists carry no secrets.
    std::string export_profiles_json() const;
    int import_profiles_json(const std::string &json); // returns number imported

private:
    std::string dir_;
};

} // namespace dbp
