// Self-update: asks the GitHub "latest release" of Db Pro, downloads the installer, verifies its SHA-256 and
// starts it silently. Opt-in (the user turns it on or presses "Check"); nothing is sent besides a plain HTTPS GET.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dbp::updater {

struct Release {
    bool ok = false;
    std::string error;
    std::string tag;
    std::string version; // tag without the leading "v"
    std::string notes;
    std::string installer_url;
    std::string installer_name;
    std::string sha256; // lowercase hex; a release without one is refused
    int64_t size = 0;
};

// Pure: builds a Release from the GitHub API body. `fetch_text` downloads a small text asset (the .sha256 file)
// when the asset list carries no digest. Exposed for tests.
Release parse_release(const std::string &body, const std::function<std::string(const std::string &)> &fetch_text);

// Queries the latest release. Blocking; call from a worker thread.
Release fetch_latest();

// True when `remote` is a higher dotted version than `local` ("0.10.0" > "0.9.2").
bool is_newer(const std::string &remote, const std::string &local);

// Downloads `url` (https, Db Pro release assets only) into `path` through a ".part" file. Returns an empty string
// on success, otherwise an error. `progress(done, total)` may report total <= 0. `cancelled` may be null.
std::string download(const std::string &url, const std::string &path, const std::function<void(int64_t, int64_t)> &progress,
                     const std::function<bool()> &cancelled = nullptr);

// Lowercase hex SHA-256 of a file, or an empty string if it cannot be read.
std::string sha256_file(const std::string &path);

// Starts the installer silently and returns at once. The caller must exit afterwards so Setup can replace the
// running files; Setup relaunches the app (/RELAUNCH=1).
bool run_installer(const std::string &path);

// Folder for downloaded installers (%TEMP%\DbPro-update).
std::string download_dir();

} // namespace dbp::updater
