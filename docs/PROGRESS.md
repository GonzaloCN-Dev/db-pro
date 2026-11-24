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
| Ajustes, pestañas y vista de rendimiento | Compilación MSVC correcta. Autotest visual en 1360×800: preferencias, selector de idioma/tema/intervalo, dos tablas SQLite abiertas en pestañas y cambio de vista. SQLite muestra correctamente el estado de monitoreo no soportado. |
| Updater | Compilación y 3 tests unitarios (comparación de versiones, validación de release/checksum/URL, vector SHA-256). El flujo real de descarga, instalación y relanzamiento no se ha ejecutado. |
| Suite | `build\dbpro_tests.exe`: **71 tests, 0 fallidos**. |
| Diagnóstico de cierres | Formateo privacy-safe, fase thread-local y sanitización del nombre de módulo cubiertos por tests. Reproducción visual en SQLite temporal: conectar y abrir tabla sin cierre. El cierre remoto SQL Server reportado no se pudo reproducir; no se considera resuelto. |
| Acerca de y ajustes | Autotest visual: copyright, versión, enlace al repo, controles de tamaño de página, idioma, tema e intervalo visibles; ajuste de filas persistido en store. |
| Pestañas | Guardar/descartar explícito en cierre de pestaña y operación que abandona todas las tablas; build y suite pasan. Falta cobertura UI específica de los flujos sucios. |
| Artefactos 0.2.0 | Build MSVC, instalador Inno Setup y paquete de 5 drivers con `drivers.json` generados; checksum SHA-256 del instalador. Instalación silenciosa y actualización end-to-end aún no verificadas. |
| Instalador 0.2.1 | Instalación silenciosa `/VERYSILENT` en directorio temporal: exit code 0, archivos completos y app instalada abre una tabla SQLite temporal y cierra con exit code 0. Actualización sobre instalación previa sigue sin probar. |

## Cómo se prueba la interfaz
`DBPRO_SELFTEST=<script>` activa `src/selftest.cpp`: inyecta clics/teclas en la ventana de Slint y guarda capturas de esa ventana (`shot`). No toca el escritorio real. Variables: `DBPRO_SELFTEST_OUT`, `DBPRO_SELFTEST_EXPORT` (ruta sin extensión, evita el diálogo nativo), `DBPRO_DATA_DIR` (perfiles aislados / modo portable).

## Sin verificar todavía
- Conflicto de edición concurrente en la UI (el test unitario del núcleo sí existe).
- Insertar / duplicar / eliminar filas, deshacer/rehacer en la UI.
- Editor SQL, importación, cancelar consulta/exportación, ventana con muchos datos (scroll horizontal con ratón real).
- Medidas de arranque en frío y de memoria con 70k filas cargadas (`docs/BENCHMARKS.md` pendiente).
- Instalador y versión portable.
- Monitor en vivo y finalización de sesiones contra un servidor PostgreSQL, MongoDB o SQL Server real. El estado no soportado de SQLite sí se verificó visualmente.
- Descarga de una release y actualización/relanzamiento con el instalador en una instalación separada.
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
