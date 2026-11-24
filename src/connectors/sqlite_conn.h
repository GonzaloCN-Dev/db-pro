#pragma once

#include <memory>

#include "../core/core.h"

namespace dbp {

// Opens (or creates) a SQLite database file. Throws DbError(Connection) on failure.
std::unique_ptr<Connector> connect_sqlite(const std::string &path, bool read_only);

} // namespace dbp
