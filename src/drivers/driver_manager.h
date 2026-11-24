// Finds, verifies, loads and installs engine drivers (dbpro_driver_<id>.dll).
// SQLite is built into the app. Every other engine lives in its own DLL together with its client libraries,
// so users only download what they actually connect to.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../core/core.h"
#include "../store.h"

namespace dbp::drivers {

enum class State {
    BuiltIn,        // part of the app (SQLite)
    Ready,          // driver found, verified and loadable
    Missing,        // implemented, but its DLL is not installed
    Incompatible,   // DLL present but rejected (see detail)
    NotImplemented  // no driver exists for this engine in this version
};

struct Status {
    State state = State::NotImplemented;
    std::string driver_id; // "" for built-in / unknown engines
    std::string detail;    // reason for Incompatible, library description for Ready
    bool usable() const { return state == State::BuiltIn || state == State::Ready; }
};

// Full check: loads the driver DLL if needed and validates ABI, version and checksum.
Status status_for_engine(const std::string &engine);

// Cheap check for lists and labels: never loads a DLL. Ready only means "a driver file is present".
Status probe_for_engine(const std::string &engine);

// Throws DbError(Unsupported) when the driver is not usable, otherwise whatever the connector throws.
std::unique_ptr<Connector> connect(const Profile &p, const std::string &password);

using ProgressFn = std::function<void(const std::string &)>;

// Downloads the driver for an engine from the release that matches this app version, verifies size and SHA-256,
// and installs it in the per-user drivers folder. Returns false and fills `error` on failure.
bool install_for_engine(const std::string &engine, const ProgressFn &progress, const CancelPtr &cancel, std::string &error);

// Per-user folder for downloaded drivers (%APPDATA%\DbPro\drivers, or DBPRO_DATA_DIR\drivers).
std::string user_drivers_dir();

// Tests only: unloads every driver. Never call while a connector created by a driver is alive.
void unload_all_for_tests();

std::string sha256_hex(const std::string &bytes);
bool sha256_file(const std::string &path, std::string &hex);

} // namespace dbp::drivers
