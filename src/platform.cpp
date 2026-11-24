#include "platform.h"

#include <windows.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincred.h>

#include <filesystem>

#include "core/brand.h"

namespace dbp::platform {

std::wstring widen(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string data_dir()
{
    PWSTR p = nullptr;
    std::filesystem::path dir;
    wchar_t over[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"DBPRO_DATA_DIR", over, MAX_PATH) > 0) {
        dir = over; // portable mode / tests
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        dir = std::filesystem::path(p) / brand::kDataDir;
        CoTaskMemFree(p);
    } else {
        dir = std::filesystem::temp_directory_path() / brand::kDataDir;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return narrow(dir.wstring());
}

bool cred_write(const std::string &target, const std::string &user, const std::string &secret)
{
    std::wstring t = widen(brand::kCredentialPrefix + target), u = widen(user);
    std::wstring blob = widen(secret);
    CREDENTIALW c {};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t.data();
    c.UserName = u.data();
    c.CredentialBlobSize = (DWORD)(blob.size() * sizeof(wchar_t));
    c.CredentialBlob = (LPBYTE)blob.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    BOOL ok = CredWriteW(&c, 0);
    SecureZeroMemory(blob.data(), blob.size() * sizeof(wchar_t));
    return ok == TRUE;
}

std::optional<std::string> cred_read(const std::string &target)
{
    std::wstring t = widen(brand::kCredentialPrefix + target);
    PCREDENTIALW c = nullptr;
    if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &c)) return std::nullopt;
    std::wstring w((const wchar_t *)c->CredentialBlob, c->CredentialBlobSize / sizeof(wchar_t));
    CredFree(c);
    std::string out = narrow(w);
    SecureZeroMemory(w.data(), w.size() * sizeof(wchar_t));
    return out;
}

void cred_delete(const std::string &target)
{
    std::wstring t = widen(brand::kCredentialPrefix + target);
    CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0);
}

bool clipboard_set(const std::string &utf8)
{
    std::wstring w = widen(utf8);
    if (!OpenClipboard(GetActiveWindow())) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (!h) { CloseClipboard(); return false; }
    memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    if (!ok) GlobalFree(h);
    CloseClipboard();
    return ok;
}

static std::wstring filter_string(const std::vector<Filter> &f)
{
    std::wstring r;
    for (auto &p : f) {
        r += widen(p.first + " (" + p.second + ")");
        r.push_back(L'\0');
        r += widen(p.second);
        r.push_back(L'\0');
    }
    r.push_back(L'\0');
    return r;
}

std::optional<std::string> save_dialog(const std::string &title, const std::string &default_name, const std::vector<Filter> &filters)
{
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring dn = widen(default_name);
    wcsncpy_s(buf, dn.c_str(), _TRUNCATE);
    std::wstring flt = filter_string(filters), ttl = widen(title);
    OPENFILENAMEW o {};
    o.lStructSize = sizeof o;
    o.hwndOwner = GetActiveWindow();
    o.lpstrFilter = flt.c_str();
    o.lpstrFile = buf;
    o.nMaxFile = (DWORD)(sizeof buf / sizeof(wchar_t));
    o.lpstrTitle = ttl.c_str();
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&o)) return std::nullopt;
    std::string path = narrow(buf);
    // Append the extension of the chosen filter when the user typed none.
    if (std::filesystem::path(path).extension().empty() && o.nFilterIndex >= 1 && o.nFilterIndex <= filters.size()) {
        std::string pat = filters[o.nFilterIndex - 1].second;
        auto dot = pat.rfind('.');
        if (dot != std::string::npos && pat.find('*', dot) == std::string::npos) path += pat.substr(dot);
    }
    return path;
}

std::optional<std::string> open_dialog(const std::string &title, const std::vector<Filter> &filters)
{
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring flt = filter_string(filters), ttl = widen(title);
    OPENFILENAMEW o {};
    o.lStructSize = sizeof o;
    o.hwndOwner = GetActiveWindow();
    o.lpstrFilter = flt.c_str();
    o.lpstrFile = buf;
    o.nMaxFile = (DWORD)(sizeof buf / sizeof(wchar_t));
    o.lpstrTitle = ttl.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return std::nullopt;
    return narrow(buf);
}

void reveal_in_explorer(const std::string &path)
{
    std::wstring arg = L"/select,\"" + widen(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace dbp::platform
