#include "driver_manager.h"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

#include "../core/brand.h"
#include "../platform.h"
#include "driver_api.h"

namespace dbp::drivers {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct DriverDef {
    const char *id;
    const char *file;
    std::vector<const char *> engines;
    bool implemented;
};

const std::vector<DriverDef> &defs()
{
    static const std::vector<DriverDef> d = {
        { "mysql", "dbpro_driver_mysql.dll", { "mysql", "mariadb" }, true },
        { "postgres", "dbpro_driver_postgres.dll", { "postgres" }, false },
        { "mssql", "dbpro_driver_mssql.dll", { "mssql" }, false },
        { "mongodb", "dbpro_driver_mongodb.dll", { "mongodb" }, false },
    };
    return d;
}

const DriverDef *def_for_engine(const std::string &engine)
{
    for (auto &d : defs())
        for (auto *e : d.engines)
            if (engine == e) return &d;
    return nullptr;
}

fs::path exe_dir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return fs::path(std::wstring(buf, n)).parent_path();
}

// Dev/test hook: replaces the folder next to the executable.
fs::path shipped_dir()
{
    wchar_t over[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"DBPRO_DRIVERS_DIR", over, MAX_PATH) > 0) return fs::path(over);
    return exe_dir() / "drivers";
}

fs::path user_dir() { return fs::u8path(platform::data_dir()) / "drivers"; }

// ------------------------------------------------------------------ loaded drivers
struct Loaded {
    HMODULE mod = nullptr;
    DbproConnectFn connect = nullptr;
    std::string library;
};

std::mutex g_mutex;
std::map<std::string, Loaded> g_loaded; // modules stay loaded: connectors created by a driver reference its code

// Hash recorded at install time. A DLL in the writable user folder must still match it before it is loaded.
fs::path installed_index() { return user_dir() / "installed.json"; }

std::string recorded_hash(const std::string &file)
{
    try {
        std::ifstream f(installed_index());
        if (!f) return {};
        json j = json::parse(f);
        return j.at("files").value(file, "");
    } catch (...) {
        return {};
    }
}

void record_hash(const std::string &file, const std::string &hex)
{
    json j = json::object();
    try {
        std::ifstream f(installed_index());
        if (f) j = json::parse(f);
    } catch (...) {
        j = json::object();
    }
    j["files"][file] = hex;
    std::ofstream o(installed_index(), std::ios::trunc);
    o << j.dump(2);
}

// Returns true and fills `out` when the DLL loads and passes the ABI and version checks.
bool load_dll(const DriverDef &d, const fs::path &path, bool trusted, Loaded &out, std::string &why)
{
    if (!trusted) {
        std::string want = recorded_hash(d.file), got;
        if (want.empty()) { why = "driver was not installed by Db Pro (no recorded checksum)"; return false; }
        if (!sha256_file(platform::narrow(path.wstring()), got) || got != want) { why = "driver file does not match the checksum recorded at install time"; return false; }
    }
    HMODULE m = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!m) { why = "cannot load driver (Windows error " + std::to_string(GetLastError()) + ")"; return false; }
    auto abi = (DbproAbiFn)GetProcAddress(m, "dbpro_driver_abi");
    auto info = (DbproInfoFn)GetProcAddress(m, "dbpro_driver_info");
    auto conn = (DbproConnectFn)GetProcAddress(m, "dbpro_driver_connect");
    if (!abi || !info || !conn) { FreeLibrary(m); why = "file is not a Db Pro driver"; return false; }
    if (abi() != DBPRO_DRIVER_ABI) { FreeLibrary(m); why = "driver ABI " + std::to_string(abi()) + " does not match this app (" + std::to_string(DBPRO_DRIVER_ABI) + ")"; return false; }
    std::string lib;
    try {
        json j = json::parse(info());
        std::string ver = j.value("app_version", "");
        if (ver != brand::kVersion) { FreeLibrary(m); why = "driver is for Db Pro " + ver + ", this is " + brand::kVersion; return false; }
        lib = j.value("library", "");
    } catch (...) {
        FreeLibrary(m);
        why = "unreadable driver information";
        return false;
    }
    out.mod = m;
    out.connect = conn;
    out.library = lib;
    return true;
}

Status resolve(const DriverDef &d)
{
    Status s;
    s.driver_id = d.id;
    if (!d.implemented) { s.state = State::NotImplemented; return s; }
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_loaded.find(d.id);
    if (it != g_loaded.end()) { s.state = State::Ready; s.detail = it->second.library; return s; }

    struct Cand { fs::path path; bool trusted; };
    Cand cands[] = { { shipped_dir() / d.file, true }, { user_dir() / d.file, false } };
    std::string last_reason;
    bool found = false;
    for (auto &c : cands) {
        std::error_code ec;
        if (!fs::is_regular_file(c.path, ec)) continue;
        found = true;
        Loaded l;
        std::string why;
        if (load_dll(d, c.path, c.trusted, l, why)) {
            g_loaded[d.id] = l;
            s.state = State::Ready;
            s.detail = l.library;
            return s;
        }
        last_reason = why;
    }
    if (found) { s.state = State::Incompatible; s.detail = last_reason; }
    else s.state = State::Missing;
    return s;
}

// ------------------------------------------------------------------ download
struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

bool http_get(const std::string &url, size_t max_bytes, const CancelPtr &cancel, const std::function<void(size_t, size_t)> &on_bytes, std::string &out, std::string &err)
{
    std::wstring w = platform::widen(url);
    URL_COMPONENTS uc {};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { err = "invalid download address"; return false; }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { err = "downloads must use HTTPS"; return false; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    std::wstring agent = platform::widen(std::string(brand::kName) + "/" + brand::kVersion);
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { err = "network unavailable"; return false; }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!conn.h) { err = "cannot reach the server"; return false; }
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) { err = "cannot create the request"; return false; }
    // Certificate validation stays on (the WinHTTP default); redirects must not downgrade to HTTP.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        err = "connection failed (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status == 404) { err = "not found on the server (HTTP 404). The release may not exist yet, or the repository is private."; return false; }
    if (status != 200) { err = "server answered HTTP " + std::to_string(status); return false; }
    DWORD len = 0;
    sz = sizeof len;
    size_t total = 0;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX)) total = len;
    if (total > max_bytes) { err = "download is larger than allowed"; return false; }

    out.clear();
    std::string chunk(64 * 1024, '\0');
    for (;;) {
        if (cancel && cancel->cancelled()) { err = "cancelled"; return false; }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), (DWORD)chunk.size(), &got)) { err = "download interrupted"; return false; }
        if (got == 0) break;
        if (out.size() + got > max_bytes) { err = "download is larger than allowed"; return false; }
        out.append(chunk.data(), got);
        if (on_bytes) on_bytes(out.size(), total);
    }
    return true;
}

std::string mb(size_t n)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    return b;
}

bool plain_file_name(const std::string &n)
{
    return !n.empty() && n.find_first_of("\\/:") == std::string::npos && n.find("..") == std::string::npos;
}

} // namespace

// ------------------------------------------------------------------ hashing
std::string sha256_hex(const std::string &bytes)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return hex;
    UCHAR dig[32];
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
        if (BCryptHashData(h, (PUCHAR)bytes.data(), (ULONG)bytes.size(), 0) >= 0 && BCryptFinishHash(h, dig, sizeof dig, 0) >= 0) {
            static const char *k = "0123456789abcdef";
            for (UCHAR b : dig) { hex += k[b >> 4]; hex += k[b & 15]; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool sha256_file(const std::string &path, std::string &hex)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    hex = sha256_hex(all);
    return !hex.empty();
}

void unload_all_for_tests()
{
    std::lock_guard<std::mutex> g(g_mutex);
    for (auto &kv : g_loaded)
        if (kv.second.mod) FreeLibrary(kv.second.mod);
    g_loaded.clear();
}

std::string user_drivers_dir() { return platform::narrow(user_dir().wstring()); }

// ------------------------------------------------------------------ public API
Status status_for_engine(const std::string &engine)
{
    if (engine == "sqlite") {
        Status s;
        s.state = State::BuiltIn;
        return s;
    }
    const DriverDef *d = def_for_engine(engine);
    if (!d) return {};
    return resolve(*d);
}

Status probe_for_engine(const std::string &engine)
{
    Status s;
    if (engine == "sqlite") {
        s.state = State::BuiltIn;
        return s;
    }
    const DriverDef *d = def_for_engine(engine);
    if (!d) return s;
    s.driver_id = d->id;
    if (!d->implemented) return s;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        if (g_loaded.count(d->id)) { s.state = State::Ready; return s; }
    }
    std::error_code ec;
    bool present = fs::is_regular_file(shipped_dir() / d->file, ec) || fs::is_regular_file(user_dir() / d->file, ec);
    s.state = present ? State::Ready : State::Missing;
    return s;
}

std::unique_ptr<Connector> connect(const Profile &p, const std::string &password)
{
    const DriverDef *d = def_for_engine(p.engine);
    if (!d) throw DbError(ErrKind::Unsupported, "Unknown engine: " + p.engine);
    Status s = resolve(*d);
    if (s.state != State::Ready) throw DbError(ErrKind::Unsupported, "The " + std::string(d->id) + " driver is not available" + (s.detail.empty() ? "" : ": " + s.detail));
    DbproConnectFn fn;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        fn = g_loaded.at(d->id).connect;
    }
    DbproConnectParams cp {};
    cp.host = p.host.c_str();
    cp.port = p.port;
    cp.user = p.user.c_str();
    cp.password = password.c_str();
    cp.database = p.database.c_str();
    cp.path = p.path.c_str();
    cp.tls = p.tls ? 1 : 0;
    cp.trust_server_cert = p.trust_server_cert ? 1 : 0;
    cp.read_only = p.read_only ? 1 : 0;
    int kind = (int)ErrKind::Other;
    char err[1024] = {};
    void *obj = fn(p.engine.c_str(), &cp, &kind, err, sizeof err);
    if (!obj) throw DbError((ErrKind)kind, err[0] ? err : "driver could not connect");
    return std::unique_ptr<Connector>(static_cast<Connector *>(obj));
}

bool install_for_engine(const std::string &engine, const ProgressFn &progress, const CancelPtr &cancel, std::string &error)
{
    const DriverDef *d = def_for_engine(engine);
    if (!d || !d->implemented) { error = "There is no driver for this engine in this version."; return false; }
    auto say = [&](const std::string &m) { if (progress) progress(m); };

    std::string base = std::string(brand::kDriversUrlBase) + "v" + brand::kVersion + "/";
    say("Looking up driver…");
    std::string body;
    if (!http_get(base + "drivers.json", 1 << 20, cancel, nullptr, body, error)) return false;

    std::string file, want;
    size_t want_size = 0;
    try {
        json m = json::parse(body);
        if (m.value("app_version", "") != brand::kVersion) { error = "The published drivers are for a different version of Db Pro."; return false; }
        const json &e = m.at("drivers").at(d->id);
        file = e.at("file").get<std::string>();
        want = e.at("sha256").get<std::string>();
        want_size = e.at("size").get<size_t>();
    } catch (...) {
        error = "The driver manifest is missing or invalid.";
        return false;
    }
    if (file != d->file || !plain_file_name(file)) { error = "The driver manifest names an unexpected file."; return false; }
    constexpr size_t kMax = 200u << 20;
    if (want_size == 0 || want_size > kMax) { error = "The driver manifest reports an invalid size."; return false; }

    std::string data;
    auto on_bytes = [&](size_t n, size_t total) { say("Downloading " + std::string(d->id) + " driver… " + mb(n) + (total ? " / " + mb(total) : "")); };
    if (!http_get(base + file, want_size, cancel, on_bytes, data, error)) return false;
    if (data.size() != want_size) { error = "The downloaded file has the wrong size."; return false; }
    say("Verifying…");
    if (sha256_hex(data) != want) { error = "Checksum mismatch: the download was discarded."; return false; }

    std::error_code ec;
    fs::create_directories(user_dir(), ec);
    fs::path dest = user_dir() / file, part = user_dir() / (file + ".part");
    {
        std::ofstream o(part, std::ios::binary | std::ios::trunc);
        o.write(data.data(), (std::streamsize)data.size());
        if (!o) { error = "Cannot write to " + platform::narrow(user_dir().wstring()); return false; }
    }
    {
        // A running process may have the old DLL mapped; the replacement then takes effect on the next start.
        std::lock_guard<std::mutex> g(g_mutex);
        if (g_loaded.count(d->id)) { fs::remove(part, ec); error = "A driver is already loaded. Restart Db Pro to replace it."; return false; }
    }
    fs::remove(dest, ec);
    fs::rename(part, dest, ec);
    if (ec) { fs::remove(part, ec); error = "Cannot install the driver file: " + ec.message(); return false; }
    record_hash(file, want);

    Status s = resolve(*d);
    if (s.state != State::Ready) { error = "The driver was installed but cannot be used: " + s.detail; return false; }
    say("Driver installed.");
    return true;
}

} // namespace dbp::drivers
