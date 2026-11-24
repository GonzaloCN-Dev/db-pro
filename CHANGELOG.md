# Changelog

## 0.1.0
- Núcleo, interfaz Slint, cuadrícula de datos editable con filtros y ordenación en servidor, exportación en streaming.
- SQLite integrado. Drivers opcionales: MySQL/MariaDB, PostgreSQL, MongoDB (probados contra servidores reales desechables) y SQL Server (compilado, sin probar contra un servidor).
- Motores compatibles (YugabyteDB, TiDB, EDB, CrateDB, ...) como experimentales sobre los drivers existentes.
- Instalador con un componente por driver; drivers descargables con verificación SHA-256.
