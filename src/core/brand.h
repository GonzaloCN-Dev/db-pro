// Single source of truth for product identity. Change here, nowhere else.
#pragma once

namespace dbp::brand {

inline constexpr const char *kName = "Db Pro";
inline constexpr const char *kTagline = "Connect. Query. Explore.";
inline constexpr const char *kAuthor = "Gonzalo Cruz Núñez";
inline constexpr const char *kCopyright = "Copyright © 2026 Gonzalo Cruz Núñez";
inline constexpr const char *kDescription = "Native universal database studio";
inline constexpr const char *kDataDir = "DbPro";          // %APPDATA%\DbPro
inline constexpr const char *kCredentialPrefix = "DbPro/"; // Windows Credential Manager target prefix

#ifdef APP_VERSION
inline constexpr const char *kVersion = APP_VERSION;
#else
inline constexpr const char *kVersion = "0.0.0";
#endif

} // namespace dbp::brand
