#pragma once

#include <memory>
#include <string>

#include "../core/core.h"

namespace dbp {

struct MongoParams {
    std::string host = "localhost"; // "host", or "mongodb+srv://cluster.example.net" for an Atlas SRV record
    unsigned port = 27017;          // ignored for SRV
    std::string user;               // empty = no authentication
    std::string password;
    std::string database;           // empty: list every database
    bool tls = true;
    bool trust_server_cert = false; // explicit opt-in: tlsAllowInvalidCertificates=true
    bool read_only = false;
};

// MongoDB through mongo-cxx-driver. Throws DbError(Connection) on failure.
std::unique_ptr<Connector> connect_mongodb(const MongoParams &p);

} // namespace dbp
