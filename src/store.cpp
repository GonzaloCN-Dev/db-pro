#include "store.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include "platform.h"

namespace dbp {

using nlohmann::json;
namespace fs = std::filesystem;

const char *env_name(Env e)
{
    switch (e) {
    case Env::Development: return "Development";
    case Env::Testing: return "Testing";
    case Env::Staging: return "Staging";
    case Env::Production: return "Production";
    }
    return "";
}

static json to_json(const Profile &p)
{
    return { { "id", p.id }, { "name", p.name }, { "engine", p.engine }, { "host", p.host }, { "port", p.port },
             { "database", p.database }, { "user", p.user }, { "path", p.path }, { "env", p.env },
             { "read_only", p.read_only }, { "tls", p.tls }, { "trust_server_cert", p.trust_server_cert } };
}

static Profile from_json(const json &j)
{
    Profile p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.engine = j.value("engine", "");
    p.host = j.value("host", "");
    p.port = j.value("port", 0);
    p.database = j.value("database", "");
    p.user = j.value("user", "");
    p.path = j.value("path", "");
    p.env = std::clamp(j.value("env", 0), 0, 3);
    p.read_only = j.value("read_only", false);
    p.tls = j.value("tls", true);
    p.trust_server_cert = j.value("trust_server_cert", false);
    return p;
}

Store::Store(std::string dir) : dir_(dir.empty() ? platform::data_dir() : std::move(dir))
{
    std::error_code ec;
    fs::create_directories(platform::widen(dir_), ec);
}

std::string Store::new_id()
{
    static const char *hex = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    for (int k = 0; k < 16; ++k) s += hex[rd() % 16];
    return s;
}

const Profile *Store::find(const std::string &id) const
{
    for (auto &p : profiles)
        if (p.id == id) return &p;
    return nullptr;
}

void Store::upsert(const Profile &p)
{
    for (auto &x : profiles)
        if (x.id == p.id) { x = p; return; }
    profiles.push_back(p);
}

void Store::remove(const std::string &id)
{
    for (size_t k = 0; k < profiles.size(); ++k)
        if (profiles[k].id == id) { profiles.erase(profiles.begin() + k); return; }
}

static std::string read_file(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void Store::load()
{
    profiles.clear();
    try {
        fs::path pf = fs::path(platform::widen(dir_)) / L"connections.json";
        if (fs::exists(pf)) {
            json j = json::parse(read_file(pf));
            for (auto &e : j.value("profiles", json::array())) {
                Profile p = from_json(e);
                if (!p.id.empty()) profiles.push_back(p);
            }
        }
        fs::path sf = fs::path(platform::widen(dir_)) / L"settings.json";
        if (fs::exists(sf)) {
            json j = json::parse(read_file(sf));
            settings.dark = j.value("dark", true);
            settings.lang = j.value("lang", "en");
            settings.page_size = std::clamp(j.value("page_size", 100), 10, 5000);
            settings.last_export_dir = j.value("last_export_dir", "");
            settings.auto_update = j.value("auto_update", false);
            settings.last_update_check = j.value("last_update_check", 0LL);
            settings.monitor_interval = std::clamp(j.value("monitor_interval", 5), 1, 60);
        }
    } catch (...) {
        // A corrupt file must not stop the app from starting; the user simply sees no profiles.
    }
}

static bool write_atomic(const fs::path &p, const std::string &data)
{
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f.good()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

bool Store::save() const
{
    json pj = json::array();
    for (auto &p : profiles) pj.push_back(to_json(p));
    json s = { { "dark", settings.dark }, { "lang", settings.lang }, { "page_size", settings.page_size }, { "last_export_dir", settings.last_export_dir },
                { "auto_update", settings.auto_update }, { "last_update_check", settings.last_update_check }, { "monitor_interval", settings.monitor_interval } };
    fs::path dir(platform::widen(dir_));
    bool a = write_atomic(dir / L"connections.json", json({ { "version", 1 }, { "profiles", pj } }).dump(2));
    bool b = write_atomic(dir / L"settings.json", s.dump(2));
    return a && b;
}

std::string Store::export_profiles_json() const
{
    json pj = json::array();
    for (auto &p : profiles) pj.push_back(to_json(p));
    return json({ { "version", 1 }, { "note", "Passwords are not exported." }, { "profiles", pj } }).dump(2);
}

int Store::import_profiles_json(const std::string &text)
{
    json j = json::parse(text);
    int n = 0;
    for (auto &e : j.value("profiles", json::array())) {
        Profile p = from_json(e);
        if (p.name.empty() || p.engine.empty()) continue;
        p.id = new_id(); // never collide with, or overwrite, an existing profile
        profiles.push_back(p);
        ++n;
    }
    return n;
}

} // namespace dbp
