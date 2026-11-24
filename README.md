# Db Pro

*Connect. Query. Explore.*

Native database studio for Windows 10/11 (C++20 + Slint). No Electron, no WebView, no telemetry.

> **Status: early development (0.1.0).** See [docs/PROGRESS.md](docs/PROGRESS.md) for what has actually been run and verified.

## Engines

| Engine | Status |
|---|---|
| SQLite | Built in. Implemented, tested on temporary files and through the UI |
| MySQL / MariaDB | Optional driver. Experimental: **not yet run against a server** |
| PostgreSQL, SQL Server, MongoDB | Not implemented yet |

Only SQLite is inside the app. Other engines use an optional driver DLL that the connection dialog offers to download when you pick that engine, so you only get the libraries you use. See [docs/DRIVERS.md](docs/DRIVERS.md).

## Build

Requires Visual Studio 2022 or later with the C++ and CMake tools, plus vcpkg.

```powershell
git clone https://github.com/microsoft/vcpkg third_party\vcpkg
third_party\vcpkg\bootstrap-vcpkg.bat
.\build.bat
build\dbpro_tests.exe
```

## Installer

```powershell
.\build.bat
cmake --build build --target dbpro_drivers
.\installer\build-installer.ps1   # needs Inno Setup 6; writes dist\DbPro-Setup-<version>.exe
```

The installer lets you choose which optional drivers to include (none are required; SQLite is built in). Per-user install by default, no administrator rights needed.

Rules and decisions for contributors: [AGENTS.md](AGENTS.md), [docs/DECISIONS.md](docs/DECISIONS.md).

## License

MIT, see [LICENSE](LICENSE). Copyright © 2026 Gonzalo Cruz Núñez.
