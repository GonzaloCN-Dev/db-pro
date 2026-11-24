# PROGRESS — qué funciona de verdad

Última actualización: 2026-11-24. Solo se anota lo que se ha ejecutado y comprobado.

## Verificado
| Área | Evidencia |
|---|---|
| Núcleo (valores, filtros tipados, generador SQL de 4 dialectos, ChangeSet, exportadores, store, seguridad SQL) | 52 tests automáticos pasan (`build\dbpro_tests.exe`) |
| Conector SQLite | tests contra ficheros temporales |
| Compilación | `DbPro.exe` enlaza (MSVC, Slint 1.19.0, backend `winit-software`) |
| Arranque | ~30 MB de RAM en reposo (medido con WorkingSet, sin carga) |
| Abrir tabla de 70.000 filas | muestra 100 filas, total «desconocido» hasta pulsar *Contar* |
| Filtro por columna en servidor | `country ~ Chile` y `email ~ user6` combinados; *Contar* da 1.569, igual que una consulta independiente (`dbpro_sqlq`) |
| Limpiar filtros, orden remoto | comprobado con capturas |
| Editar celda → Guardar | diálogo con el SQL exacto (`UPDATE … WHERE id=2 AND name='Customer 2'`); tras aplicar, la base contiene el cambio y solo esa fila cambió (verificado con `dbpro_sqlq`) |
| Exportar todo lo filtrado (CSV) | 10.000 filas, todas con `country=Chile`, igual que el recuento independiente; BOM UTF-8; sin inyección de fórmulas |

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
| MySQL / MariaDB | **Experimental.** Conector escrito (`mysql_conn.cpp`, libmariadb, sentencias preparadas, KILL QUERY para cancelar) y registrado en la interfaz. Compila y los 52 tests siguen pasando, pero **no se ha ejecutado contra ningún servidor**. Hay un MariaDB local, pero harían falta credenciales. |
| PostgreSQL, SQL Server, MongoDB | **No implementados**; sin servidor para probar en esta máquina. |

La interfaz marca «(aún no disponible)» en los motores sin conector.
