# PROGRESS — qué funciona de verdad

Última actualización: 2026-11-24. Solo se anota lo que se ha ejecutado y comprobado.

## Verificado
| Área | Evidencia |
|---|---|
| Núcleo (valores, filtros tipados, generador SQL de 4 dialectos, ChangeSet, exportadores, store, seguridad SQL) | 52 tests automáticos pasan (`build\dbpro_tests.exe`) |
| Conector SQLite | tests contra ficheros temporales |
| Drivers opcionales (DLL por motor) | 6 tests: SHA-256, driver ausente, DLL ajena sin checksum rechazada, checksum incorrecto rechazado, driver real carga y devuelve error de conexión claro. UI: el diálogo muestra «driver por descargar» y el botón de instalar; la descarga devuelve el error 404 esperado porque aún no hay release |
| Compilación | `DbPro.exe` enlaza (MSVC, Slint 1.19.0, backend `winit-software`) |
| Arranque | ~30 MB de RAM en reposo (medido con WorkingSet, sin carga) |
| Abrir tabla de 70.000 filas | muestra 100 filas, total «desconocido» hasta pulsar *Contar* |
| Filtro por columna en servidor | `country ~ Chile` y `email ~ user6` combinados; *Contar* da 1.569, igual que una consulta independiente (`dbpro_sqlq`) |
| Limpiar filtros, orden remoto | comprobado con capturas |
| Editar celda → Guardar | diálogo con el SQL exacto (`UPDATE … WHERE id=2 AND name='Customer 2'`); tras aplicar, la base contiene el cambio y solo esa fila cambió (verificado con `dbpro_sqlq`) |
| Exportar todo lo filtrado (CSV) | 10.000 filas, todas con `country=Chile`, igual que el recuento independiente; BOM UTF-8; sin inyección de fórmulas |

| Instalador (Inno Setup, como Rclone Lite) | `installer\build-installer.ps1` genera `dist\DbPro-Setup-0.1.0.exe` (9,7 MB). Probado: instalación silenciosa en carpeta temporal, archivos y registro correctos, la app instalada arranca y detecta el driver de `drivers\` sin descargar, desinstalación limpia (carpeta, registro y acceso directo). No probado: instalación en el Program Files con permisos de administrador, ni actualizar sobre una versión anterior |
| Usabilidad UI | `tests\ui\run-selftests.ps1`: core, persistencia de ajustes y error de conexión pasan en perfiles/SQLite temporales. Comprueba pestañas sucias y descarte, cambio de tamaño de página, Acerca de, monitor local y persistencia tras reiniciar. |
| Updater | Compilación y 3 tests unitarios (comparación de versiones, validación de release/checksum/URL, vector SHA-256). El flujo real de descarga, instalación y relanzamiento no se ha ejecutado. |
| Monitor | Búsqueda, solo bloqueadas, orden asc/desc con desempate estable por ID y selección por identidad. 4 tests de modelo para filtro/orden/nulos. La tabla de sesiones no se verificó contra servidor real. |
| Actividad local | SQLite expone uso de caché/esquema/statement y contadores `sqlite3_db_status`; DuckDB expone memoria y almacenamiento temporal vía `duckdb_memory()`. No se inventan sesiones ni se permite terminar sesiones locales. |
| Suite | `build\dbpro_tests.exe`: **76 tests, 0 fallidos**. DuckDB 1.5.6: **91/91** integración local. |
| Diagnóstico de cierres | Formateo privacy-safe, fase thread-local y sanitización del nombre de módulo cubiertos por tests. Reproducción visual en SQLite temporal: conectar y abrir tabla sin cierre. El cierre remoto SQL Server reportado no se pudo reproducir; no se considera resuelto. |
| Acerca de y ajustes | Autotest visual: «Derechos de autor © 2026 Gonzalo Cruz Núñez» visible permanentemente en la barra inferior y destacado en Acerca de; controles de tamaño de página, idioma, tema e intervalo visibles. |
| Pestañas | Guardar/descartar explícito en cierre de pestaña. El autotest verifica dirty guard, descarte y cierre; no automatiza todavía el guardado efectivo ni cancelar. |
| Artefactos 0.2.0 | Build MSVC, instalador Inno Setup y paquete de 5 drivers con `drivers.json` generados; checksum SHA-256 del instalador. Instalación silenciosa y actualización end-to-end aún no verificadas. |
| Instalador/actualización 0.2.2 | Instalación 0.2.0 y actualización silenciosa 0.2.2 sobre la misma carpeta temporal; versión del EXE cambia correctamente y una carpeta de datos de usuario separada permanece intacta. El instalador reserva 50 MiB adicionales y el archivo `*-space.txt` informa ≈111 MiB para el paquete actual. |
| Portable 0.2.2 | ZIP extraído en carpeta temporal; launcher `DbPro-Portable.cmd` inicia la app, conecta a SQLite de la carpeta `data` y abre tabla (autotest PASS). Contraseñas no portables: Credential Manager sigue ligado al usuario Windows. |
| Firma | No se encontró certificado Authenticode válido en CurrentUser/LocalMachine ni `signtool` en PATH. El script de firma verifica certificado/chain/firma; artefacto actual no firmado. |

## Cómo se prueba la interfaz
`DBPRO_SELFTEST=<script>` activa `src/selftest.cpp`: inyecta clics/teclas en la ventana de Slint y guarda capturas de esa ventana (`shot`). No toca el escritorio real. Variables: `DBPRO_SELFTEST_OUT`, `DBPRO_SELFTEST_EXPORT` (ruta sin extensión, evita el diálogo nativo), `DBPRO_DATA_DIR` (perfiles aislados / modo portable).

## Sin verificar todavía
- Conflicto de edición concurrente en la UI (el test unitario del núcleo sí existe).
- Insertar / duplicar / eliminar filas, deshacer/rehacer en la UI.
- Editor SQL, importación, cancelar consulta/exportación, ventana con muchos datos (scroll horizontal con ratón real).
- Medidas de arranque en frío y de memoria con 70k filas cargadas (`docs/BENCHMARKS.md` pendiente).
- Monitor de sesiones, orden/filtros y permisos contra servidores PostgreSQL/MySQL/MariaDB/MongoDB/SQL Server reales; SQLite/DuckDB muestran métricas locales y no tienen sesiones de servidor.
- Descarga de una release y actualización/relanzamiento con el instalador en una instalación separada.
- Firma del instalador: requiere un certificado Authenticode de confianza con clave privada y cadena válida; no disponible aquí.
- La finalización real de tareas de drivers al cerrar se espera sin límite para evitar UAF; un conector bloqueado podría retrasar la salida. Se debe validar y mejorar con cancelación/timeouts por driver.

## Motores
| Motor | Estado |
|---|---|
| SQLite | Implementado. Probado con tests y UI sobre ficheros temporales. |
| MySQL / MariaDB | Integración **58/58** contra un MariaDB 10.4.32 real desechable (`tools/itest.cpp`). MySQL propiamente dicho sin probar. |
| PostgreSQL | Integración **59/59** contra PostgreSQL 17.5 real desechable. |
| MongoDB | Integración **61/61** contra MongoDB 8.0.9 standalone real desechable (	ools/itest_mongo.cpp). Sin probar: Atlas/SRV, auth, TLS, transacciones (réplica). |
| SQL Server | Compila; error de conexión comprobado. **Funcionalidad sin probar:** no hay servidor SQL Server. |
| YugabyteDB, EDB, Cloudberry, Greengage, Materialize, RisingWave, CrateDB, Kingbase, Fujitsu PG, Yellowbrick (driver PG); TiDB, OceanBase (driver MySQL); Babelfish (driver MSSQL) | Experimentales: solo reutilizan el driver compatible, sin probar contra sus servidores. |

La interfaz marca '(driver por descargar)' los motores cuyo driver no está instalado.
