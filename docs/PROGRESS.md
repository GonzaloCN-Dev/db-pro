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

## Cómo se prueba la interfaz
`DBPRO_SELFTEST=<script>` activa `src/selftest.cpp`: inyecta clics/teclas en la ventana de Slint y guarda capturas de esa ventana (`shot`). No toca el escritorio real. Variables: `DBPRO_SELFTEST_OUT`, `DBPRO_SELFTEST_EXPORT` (ruta sin extensión, evita el diálogo nativo), `DBPRO_DATA_DIR` (perfiles aislados / modo portable).

## Sin verificar todavía
- Conflicto de edición concurrente en la UI (el test unitario del núcleo sí existe).
- Insertar / duplicar / eliminar filas, deshacer/rehacer en la UI.
- Editor SQL, importación, cancelar consulta/exportación, ventana con muchos datos (scroll horizontal con ratón real).
- Medidas de arranque en frío y de memoria con 70k filas cargadas (`docs/BENCHMARKS.md` pendiente).
- Instalador y versión portable.

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
