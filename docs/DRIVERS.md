# Drivers

Db Pro ships SQLite inside the app. Every other engine needs a **driver**: a DLL `dbpro_driver_<id>.dll` that contains the connector and its client library. The connection dialog detects whether the selected engine's driver is present and offers to download it, so nothing is fetched for engines you do not use.

| Engine | Driver id | Status |
|---|---|---|
| SQLite | built in | Implemented and tested |
| MySQL, MariaDB, TiDB, OceanBase | `mysql` | MariaDB tested against a real server (58/58); MySQL, TiDB, OceanBase untested |
| PostgreSQL, YugabyteDB, EDB, Cloudberry, Greengage, Materialize, RisingWave, CrateDB, Kingbase, Fujitsu PG, Yellowbrick | `postgres` | PostgreSQL tested against a real server (59/59); the others are wire-compatible and untested |
| SQL Server, Babelfish | `mssql` | Compile-tested only (no server available) |
| MongoDB | `mongodb` | Tested against a real standalone server (61/61) |

## How the app finds a driver
1. `<folder of DbPro.exe>\drivers\` (portable and installer builds can bundle drivers here).
2. `%APPDATA%\DbPro\drivers\` (downloaded). Loaded only if its SHA-256 matches `installed.json`, written by Db Pro at install time.

A driver is rejected, with the reason shown in the dialog, if its ABI number or app version differs from the app.

## Downloading
The button fetches `https://github.com/GonzaloCN-Dev/db-pro/releases/download/v<version>/drivers.json`, then the DLL it names. It checks HTTPS, the file name, the size and the SHA-256 before installing. While the repository is private the download returns HTTP 404 (anonymous access); it works once the repository and release are public, or by copying the DLL manually into `drivers\`.

## Publishing drivers for a release
```powershell
.\build.bat
cmake --build build --target dbpro_drivers
.\scripts\package-drivers.ps1          # writes dist\drivers\*.dll and drivers.json
gh release create v0.1.0 dist\drivers\* --title "Db Pro 0.1.0"
```

## Adding an engine
1. Write the connector (`src/connectors/<x>_conn.cpp`) and a driver entry file like `src/drivers/mysql_driver.cpp`.
2. Add the target in `CMakeLists.txt` and to `dbpro_drivers`.
3. Mark it `implemented = true` in `src/drivers/driver_manager.cpp`.
The libraries are listed in `vcpkg.json` for developers building everything; end users only get the DLLs they choose.