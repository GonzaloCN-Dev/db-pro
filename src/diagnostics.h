#pragma once

#include <cstdint>
#include <string>

namespace dbp::diagnostics {

enum class Phase : uint32_t {
    Idle = 0,
    Connecting,
    ListingObjects,
    LoadingTableMetadata,
    FetchingPage,
    RenderingPage,
    SavingChanges,
    Monitoring,
    Exporting,
};

void set_phase(Phase phase) noexcept;
Phase current_phase() noexcept;

// Stable, privacy-safe record formatter. Never pass server, query, row, or credential data here.
std::string format_exception_record(uint32_t code, uintptr_t address, const std::string &module, Phase phase);

// What the previous run left behind. bnormal means it never reached a clean exit (fast-fail, Rust panic,
// process killed, power loss...), which no in-process handler can observe.
struct PreviousSession {
    bool abnormal = false;
    std::string version; // sanitised
    Phase phase = Phase::Idle;
};

std::string format_previous_session_record(const PreviousSession &p);

// Opens the session marker in dir (a small memory-mapped file updated on every phase change, no I/O cost),
// reports how the previous run ended and marks this one as running. Returns what it found.
PreviousSession begin_session(const std::string &dir, const std::string &version);

// Marks the session as ended normally. Call when the app returns from its event loop.
void end_session_clean() noexcept;

// Installs process-level exception and terminate handlers. Writes only exception metadata
// and the last operation phase under the app's local data directory.
void install_crash_handler();

} // namespace dbp::diagnostics
