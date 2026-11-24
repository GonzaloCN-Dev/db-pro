#pragma once

#include <memory>
#include <string>

#include "../core/core.h"

namespace dbp {

struct MySqlParams {
    std::string host = "localhost";
    unsigned port = 3306;
    std::string user;
    std::string password;
    std::string database; // optional: restricts the explorer to this database
    bool tls = true;      // TLS required and the certificate validated
    bool trust_server_cert = false; // explicit opt-in: TLS without certificate validation
    bool read_only = false;
};

// MySQL and MariaDB through libmariadb. Throws DbError(Connection) when the server cannot be reached.
std::unique_ptr<Connector> connect_mysql(const MySqlParams &p);

} // namespace dbp
