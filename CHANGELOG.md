# Changelog

## 0.2.0
- Ajustes persistentes para tema, idioma, actualizaciones opcionales e intervalo del monitor.
- Comprobación y descarga de actualizaciones desde GitHub Releases; verificación SHA-256 e instalación silenciosa con relanzamiento.
- Varias tablas en pestañas, con filtros, orden, página y cambios pendientes independientes.
- Vista de rendimiento en vivo para PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server; muestra las capacidades disponibles y requiere confirmación para terminar sesiones.
- Driver DuckDB probado contra DuckDB 1.5.6 (88 comprobaciones).
- Suite: 67 tests unitarios. Las funciones nuevas de monitor y actualización aún no se han probado end-to-end contra servidores reales ni con una actualización publicada.

## 0.1.0
- Núcleo, interfaz Slint, cuadrícula de datos editable con filtros y ordenación en servidor, exportación en streaming.
- SQLite integrado. Drivers opcionales: MySQL/MariaDB, PostgreSQL, MongoDB (probados contra servidores reales desechables) y SQL Server (compilado, sin probar contra un servidor).
- Motores compatibles (YugabyteDB, TiDB, EDB, CrateDB, ...) como experimentales sobre los drivers existentes.
- Instalador con un componente por driver; drivers descargables con verificación SHA-256.
