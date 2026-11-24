# Db Pro

*Connect. Query. Explore.*

Native database studio for Windows 10/11 (C++20 + Slint). No Electron, no WebView, no telemetry.

> **Status: early development (0.1.0).** See [docs/PROGRESS.md](docs/PROGRESS.md) for what has actually been run and verified.

## Engines

| Engine | Status |
|---|---|
| SQLite | Implemented, tested on temporary files and through the UI |
| MySQL / MariaDB | Experimental: compiles, **not yet run against a server** |
| PostgreSQL, SQL Server, MongoDB | Not implemented yet |

## Build

Requires Visual Studio 2022 or later with the C++ and CMake tools, plus vcpkg.

```powershell
git clone https://github.com/microsoft/vcpkg third_party\vcpkg
third_party\vcpkg\bootstrap-vcpkg.bat
.\build.bat
build\dbpro_tests.exe
```

Rules and decisions for contributors: [AGENTS.md](AGENTS.md), [docs/DECISIONS.md](docs/DECISIONS.md).

## License

MIT, see [LICENSE](LICENSE). Copyright © 2026 Gonzalo Cruz Núñez.
