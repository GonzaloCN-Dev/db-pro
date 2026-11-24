// Conservative classification of SQL text, used only to decide whether to ask for confirmation.
// It never decides what is allowed to run: read-only connections are enforced by the connector.
#pragma once

#include <string>

namespace dbp {

// True only when every statement starts with SELECT / WITH (without a data-changing keyword) / EXPLAIN / SHOW / DESCRIBE.
// Anything unrecognised counts as "may modify data".
bool statement_is_read_only(const std::string &sql);

} // namespace dbp
