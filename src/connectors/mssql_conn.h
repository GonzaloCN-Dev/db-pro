#pragma once

#include <memory>
#include <string>

#include "../core/core.h"

namespace dbp {

struct MsSqlParams {
    std::string host = "localhost"; // "host" or "host\INSTANCE"
    unsigned port = 1433;           // 0 = let the driver resolve a named instance
    std::string user;               // empty = Windows integrated authentication
    std::string password;
    std::string database;           // empty: list every database
    bool tls = true;                // Encrypt=yes, certificate validated
    bool trust_server_cert = false; // explicit opt-in: TrustServerCertificate=yes
    bool read_only = false;
};

// SQL Server and Azure SQL through "ODBC Driver 18 for SQL Server". Throws DbError(Connection) on failure.
std::unique_ptr<Connector> connect_mssql(const MsSqlParams &p);

} // namespace dbp
