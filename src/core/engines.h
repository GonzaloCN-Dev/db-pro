// Catalog of every engine Db Pro can offer. One row per engine the user can pick in the connection dialog.
// An engine is listed only when a real route to it exists in this version (its own driver, or a driver for a
// wire-compatible protocol). Engines that are only planned live in docs/DATABASE_COMPATIBILITY.md, never here.
#pragma once

#include <string>
#include <vector>

namespace dbp {

enum class EngineStatus {
    Tested,       // integration tests passed against a real server of this exact engine
    Experimental  // reuses a compatible driver or was never run against a real server: may fail or misbehave
};

struct EngineDef {
    const char *id;     // stored in profiles: never rename
    const char *label;  // shown to the user
    const char *driver; // "" = built into the app (SQLite), otherwise the driver id (dbpro_driver_<id>.dll)
    int port;           // default port, 0 when not network based
    EngineStatus status;
    const char *note;   // translation key of a short, honest caveat shown in the connection dialog ("" when none)
};

const std::vector<EngineDef> &engine_catalog();
const EngineDef *find_engine(const std::string &id);

// Entries whose label contains `filter` (case-insensitive), in display order. Empty filter = all.
std::vector<const EngineDef *> filter_engines(const std::string &filter);

} // namespace dbp
