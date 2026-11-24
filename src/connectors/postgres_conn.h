#pragma once

#include <memory>
#include <string>

#include "../core/core.h"

namespace dbp {

struct PgParams {
    std::string host = "localhost";
    unsigned port = 5432;
    std::string user;
    std::string password;
    std::string database; // empty: connect to "postgres" and list every database
    bool tls = true;      // TLS required and the certificate validated
    bool trust_server_cert = false; // explicit opt-in: TLS without certificate validation
    bool read_only = false;
};

// PostgreSQL through libpq. Throws DbError(Connection) when the server cannot be reached.
std::unique_ptr<Connector> connect_postgres(const PgParams &p);

} // namespace dbp
