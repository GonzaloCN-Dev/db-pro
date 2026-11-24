# Db Pro 0.2.0

Segunda versión de desarrollo: ajustes, pestañas para tablas, monitor de actividad en vivo y actualizaciones opcionales.

## Novedades

- **Ajustes:** idioma, tema, intervalo del monitor y acceso a carpetas de datos y drivers.
- **Actualizador opcional:** consulta GitHub Releases solo al solicitarla o al habilitar la búsqueda diaria. Rechaza releases sin SHA-256, verifica el instalador antes de ejecutarlo y pide al instalador que reinicie Db Pro.
- **Pestañas de tablas:** conserva página, filtros, orden, anchos y cambios pendientes; cerrar tablas con cambios requiere confirmación.
- **Rendimiento en vivo:** tarjetas con métricas e historial, lista de sesiones/bloqueos y opción confirmada de terminar sesión para conectores que la implementan. Los motores no compatibles se identifican claramente.
- **Driver DuckDB:** integración probada (88 comprobaciones).

## Verificación y límites conocidos

- Build MSVC y CTest: 67 tests, 0 fallidos.
- La interfaz de ajustes y las pestañas se comprobaron con el autotest visual.
- El estado no compatible del monitor se comprobó en SQLite. Monitor y finalización de sesiones aún requieren pruebas contra servidores reales.
- No se ha verificado end-to-end una descarga, instalación ni relanzamiento de actualización.
- SQL Server continúa siendo experimental y sin pruebas contra un servidor real.

## Instalación

Descarga `DbPro-Setup-0.2.0.exe` y ejecútalo. El instalador no está firmado; Windows SmartScreen puede mostrar un aviso de editor desconocido.

Código fuente y artefactos: https://github.com/GonzaloCN-Dev/db-pro/releases/tag/v0.2.0

Primera versión de desarrollo: estudio de bases de datos nativo para Windows, con SQLite incluido y drivers opcionales por motor.

## Novedades

- **Rejilla de datos** con paginación en servidor (100 filas por página), filtros tipados por columna aplicados sobre toda la tabla y orden remoto.
- **Total bajo petición:** sin `COUNT(*)` automático; el botón *Contar* calcula el total exacto.
- **Edición con control de conflictos:** SQL exacto antes de guardar, cambios en bloque y rechazo si otra persona modificó la fila.
- **Exportación en bloques** de todas las filas filtradas (CSV, TSV, JSON, NDJSON, SQL INSERT).
- **Editor SQL** con cancelación y exportación del resultado.
- **Conexiones:** contraseñas en el Administrador de credenciales de Windows, modo de solo lectura y etiqueta de entorno.
- **Drivers opcionales:** el diálogo de conexión detecta qué driver necesita el motor elegido y ofrece descargarlo (HTTPS, SHA-256).
- **Instalador** con selección de drivers y desinstalación limpia.
- Interfaz en español e inglés; temas oscuro y claro.

## Estado de los motores

| Motor | Estado |
| --- | --- |
| SQLite | Implementado y probado |
| MySQL / MariaDB | Probado contra MariaDB 10.4 real (MySQL sin probar) |
| PostgreSQL | Probado contra PostgreSQL 17.5 real |
| MongoDB | Probado contra MongoDB 8.0 real (standalone) |
| SQL Server / Azure SQL | Experimental: sin probar contra un servidor |
| YugabyteDB, TiDB, EDB, CrateDB, Cloudberry, Greengage, Materialize, RisingWave, Kingbase, Fujitsu PG, Yellowbrick, OceanBase, Babelfish | Experimentales (protocolo compatible), sin probar |

## Instalar

Descarga `DbPro-Setup-0.1.0.exe` y ejecútalo. El instalador no está firmado, así que Windows SmartScreen puede mostrar un aviso de editor desconocido.

## Notas

- Windows 10/11 de 64 bits.
- Interfaz en español e inglés.
- Licencia MIT para el código original de Db Pro; consulta `installer/LICENSES.txt` para los componentes de terceros.
- Creado por Gonzalo Cruz Núñez.
