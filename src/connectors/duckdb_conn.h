#pragma once

#include <memory>

#include "../core/core.h"

namespace dbp {

// Opens (or creates) a DuckDB database file; ":memory:" opens a private in-memory database.
// Throws DbError(Connection) on failure.
std::unique_ptr<Connector> connect_duckdb(const std::string &path, bool read_only);

} // namespace dbp
