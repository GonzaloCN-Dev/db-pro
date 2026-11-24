#pragma once

#include <memory>
#include <string>

#include "../core/core.h"
#include "../store.h"

namespace dbp {

// Engines this build can really connect to. The UI only offers these.
bool engine_available(const std::string &id);

// Throws DbError(Connection) when the server cannot be reached, DbError(Unsupported) for an unknown engine.
std::unique_ptr<Connector> make_connector(const Profile &p, const std::string &password);

} // namespace dbp
