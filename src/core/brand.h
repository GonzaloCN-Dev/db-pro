// Single source of truth for product identity. Change here, nowhere else.
#pragma once

namespace dbp::brand {

inline constexpr const char *kName = "Db Pro";
inline constexpr const char *kTagline = "Connect. Query. Explore.";
inline constexpr const char *kAuthor = "Gonzalo Cruz Núñez";
inline constexpr const char *kCopyright = "Derechos de autor © 2026 Gonzalo Cruz Núñez";
inline constexpr const char *kRepositoryUrl = "https://github.com/GonzaloCN-Dev";
inline constexpr const char *kDescription = "Native universal database studio";
inline constexpr const char *kDataDir = "DbPro";          // %APPDATA%\DbPro
inline constexpr const char *kCredentialPrefix = "DbPro/"; // Windows Credential Manager target prefix

// Where optional drivers are published. Layout: <base>v<version>/drivers.json and <base>v<version>/dbpro_driver_<id>.dll
inline constexpr const char *kDriversUrlBase = "https://github.com/GonzaloCN-Dev/db-pro/releases/download/";

#ifdef APP_VERSION
inline constexpr const char *kVersion = APP_VERSION;
#else
inline constexpr const char *kVersion = "0.0.0";
#endif

} // namespace dbp::brand
