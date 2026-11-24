// Contract between Db Pro and a driver DLL (dbpro_driver_<id>.dll).
// A driver bundles one engine family together with its client libraries, so the base app ships none of them.
// The Connector interface crosses the DLL boundary as a C++ object: app and drivers must be built together
// (same compiler, same CRT, same version). The loader enforces the ABI number and the app version.
#pragma once

#include <cstddef>

#define DBPRO_DRIVER_ABI 2

#ifdef _WIN32
#define DBPRO_EXPORT extern "C" __declspec(dllexport)
#else
#define DBPRO_EXPORT extern "C"
#endif

struct DbproConnectParams {
    const char *host;
    int port;
    const char *user;
    const char *password;
    const char *database;
    const char *path;
    int tls;
    int trust_server_cert;
    int read_only;
};

extern "C" {
// Exports every driver DLL provides:
//   int         dbpro_driver_abi();
//   const char *dbpro_driver_info();   // JSON {"id","engines":[],"abi","app_version","library"}
//   void       *dbpro_driver_connect(engine, params, &err_kind, err, err_size);  // dbp::Connector* or nullptr
typedef int (*DbproAbiFn)();
typedef const char *(*DbproInfoFn)();
typedef void *(*DbproConnectFn)(const char *engine, const DbproConnectParams *params, int *err_kind, char *err, size_t err_size);
}
