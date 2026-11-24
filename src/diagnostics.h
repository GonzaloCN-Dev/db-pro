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

// Installs process-level exception and terminate handlers. Writes only exception metadata
// and the last operation phase under the app's local data directory.
void install_crash_handler();

} // namespace dbp::diagnostics
