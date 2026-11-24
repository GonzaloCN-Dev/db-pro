#include "updater.h"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/brand.h"
#include "platform.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dbp::updater {

namespace {

constexpr char kLatestApi[] = "https://api.github.com/repos/GonzaloCN-Dev/db-pro/releases/latest";
constexpr char kAssetPrefix[] = "https://github.com/GonzaloCN-Dev/db-pro/releases/download/";
constexpr size_t kMaxBody = 4 * 1024 * 1024;

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v = nullptr) : h(v) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

std::string win_error(const char *what) { return std::string(what) + " (error " + std::to_string(GetLastError()) + ")"; }

// Streams a GET response to `sink`; `sink` returns false to abort. https only.
std::string http_get(const std::string &url, bool as_json, const std::function<bool(const char *, size_t)> &sink,
                     const std::function<void(int64_t)> &on_length = nullptr)
{
    std::wstring wurl = platform::widen(url);
    URL_COMPONENTSW uc {};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "invalid URL";
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return "only https connections are allowed";

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    std::wstring agent = L"DbPro/" + platform::widen(brand::kVersion);
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return win_error("cannot start WinHTTP");
    WinHttpSetTimeouts(session.h, 10000, 10000, 20000, 30000);

    Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!conn.h) return win_error("cannot connect");
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return win_error("cannot create the request");

    if (as_json) WinHttpAddRequestHeaders(req.h, L"Accept: application/vnd.github+json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr))
        return win_error("no connection to the server");

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return "the server answered " + std::to_string(status);

    if (on_length) {
        wchar_t len[32] = {};
        DWORD ls = sizeof(len);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &ls, WINHTTP_NO_HEADER_INDEX))
            on_length(_wtoi64(len));
    }

    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) return win_error("the download was interrupted");
        if (got == 0) break;
        if (!sink(buf.data(), got)) return "cancelled";
    }
    return "";
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool is_hex64(const std::string &s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::vector<int> parse_version(const std::string &v)
{
    std::vector<int> out;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    int cur = 0;
    bool any = false;
    for (; i < v.size(); ++i) {
        if (std::isdigit((unsigned char)v[i])) {
            cur = cur * 10 + (v[i] - '0');
            any = true;
        } else if (v[i] == '.') {
            out.push_back(cur);
            cur = 0;
            any = false;
        } else {
            break; // pre-release suffix such as "-beta"
        }
    }
    if (any) out.push_back(cur);
    return out;
}

bool installed_under_program_files()
{
    wchar_t self[MAX_PATH], pf[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
    for (const wchar_t *var : { L"ProgramFiles", L"ProgramFiles(x86)" }) {
        DWORD n = GetEnvironmentVariableW(var, pf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        std::wstring root = pf, me = self;
        if (me.size() > root.size() && _wcsnicmp(me.c_str(), root.c_str(), root.size()) == 0) return true;
    }
    return false;
}

std::string jstr(const json &j, const char *k)
{
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

bool is_newer(const std::string &remote, const std::string &local)
{
    auto a = parse_version(remote), b = parse_version(local);
    if (a.empty()) return false;
    size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
}

Release parse_release(const std::string &body, const std::function<std::string(const std::string &)> &fetch_text)
{
    Release r;
    json j = json::parse(body, nullptr, false);
    if (!j.is_object()) { r.error = "invalid answer from GitHub"; return r; }
    if (j.value("draft", false) || j.value("prerelease", false)) { r.error = "no stable release is published"; return r; }
    r.tag = jstr(j, "tag_name");
    r.version = (!r.tag.empty() && (r.tag[0] == 'v' || r.tag[0] == 'V')) ? r.tag.substr(1) : r.tag;
    r.notes = jstr(j, "body");
    if (parse_version(r.version).empty()) { r.error = "the published version is not valid"; return r; }

    std::string sha_url;
    auto assets = j.find("assets");
    if (assets != j.end() && assets->is_array()) {
        for (const auto &a : *assets) {
            std::string name = jstr(a, "name"), url = jstr(a, "browser_download_url");
            if (url.rfind(kAssetPrefix, 0) != 0) continue;
            std::string lname = to_lower(name);
            if (lname.rfind("dbpro-setup-", 0) == 0 && lname.size() > 4 && lname.substr(lname.size() - 4) == ".exe") {
                r.installer_name = name;
                r.installer_url = url;
                r.size = a.value("size", (int64_t)0);
                std::string digest = to_lower(jstr(a, "digest"));
                if (digest.rfind("sha256:", 0) == 0 && is_hex64(digest.substr(7))) r.sha256 = digest.substr(7);
            } else if (lname.size() > 7 && lname.substr(lname.size() - 7) == ".sha256") {
                sha_url = url;
            }
        }
    }
    if (r.installer_url.empty()) { r.error = "release " + r.tag + " has no installer"; return r; }
    if (r.sha256.empty() && !sha_url.empty() && fetch_text) {
        std::string first = to_lower(fetch_text(sha_url).substr(0, 64));
        if (is_hex64(first)) r.sha256 = first;
    }
    if (r.sha256.empty()) { r.error = "release " + r.tag + " publishes no SHA-256 to verify the download"; return r; }
    r.ok = true;
    return r;
}

Release fetch_latest()
{
    Release r;
    std::string body;
    std::string err = http_get(kLatestApi, true, [&](const char *p, size_t n) {
        if (body.size() + n > kMaxBody) return false;
        body.append(p, n);
        return true;
    });
    if (!err.empty()) { r.error = err; return r; }
    return parse_release(body, [](const std::string &url) {
        std::string text;
        http_get(url, false, [&](const char *p, size_t n) {
            if (text.size() + n > 4096) return false;
            text.append(p, n);
            return true;
        });
        return text;
    });
}

std::string download_dir()
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    fs::path p = (n && n < MAX_PATH) ? fs::path(tmp) : fs::temp_directory_path();
    return platform::narrow((p / L"DbPro-update").wstring());
}

std::string download(const std::string &url, const std::string &path, const std::function<void(int64_t, int64_t)> &progress,
                     const std::function<bool()> &cancelled)
{
    if (url.rfind(kAssetPrefix, 0) != 0) return "download source not allowed";
    fs::path target = fs::path(platform::widen(path));
    fs::path part = target;
    part += L".part";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::remove(part, ec);

    std::string err;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot create the temporary file";
        int64_t total = 0, done = 0;
        err = http_get(
            url, false,
            [&](const char *p, size_t n) {
                if (cancelled && cancelled()) return false;
                out.write(p, (std::streamsize)n);
                if (!out) return false;
                done += (int64_t)n;
                if (progress) progress(done, total);
                return true;
            },
            [&](int64_t len) { total = len; });
        out.close();
        if (err.empty() && !out) err = "cannot write to disk";
    }
    if (!err.empty()) { fs::remove(part, ec); return err; }
    fs::remove(target, ec);
    fs::rename(part, target, ec);
    if (ec) return "cannot save the installer: " + ec.message();
    return "";
}

std::string sha256_file(const std::string &path)
{
    std::ifstream in(fs::path(platform::widen(path)), std::ios::binary);
    if (!in) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "";
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<char> buf(256 * 1024);
        bool ok = true;
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            auto got = (ULONG)in.gcount();
            if (got && BCryptHashData(hash, (PUCHAR)buf.data(), got, 0) < 0) { ok = false; break; }
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0) {
            static const char *hex = "0123456789abcdef";
            for (UCHAR b : digest) {
                result += hex[b >> 4];
                result += hex[b & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

bool run_installer(const std::string &path)
{
    std::wstring cmd = L"\"" + platform::widen(path) + L"\" /SILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CLOSEAPPLICATIONS /RELAUNCH=1 ";
    cmd += installed_under_program_files() ? L"/ALLUSERS" : L"/CURRENTUSER";
    STARTUPINFOW si {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi {};
    // Setup must outlive us: break away from any job object that would kill children on exit.
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace dbp::updater
