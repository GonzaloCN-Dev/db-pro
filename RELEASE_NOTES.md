# Db Pro 0.2.7

## Mejoras

- **Columnas:** botón «Columnas» para ocultar y reordenar. Se guarda por conexión y tabla (por nombre de columna, así que un cambio de esquema no oculta la columna equivocada). Solo cambia la vista: edición, orden, filtros y guardado usan las columnas reales. Siempre queda una columna visible y no se puede ocultar una con filtro activo.
- **Copiar como:** TSV, CSV, JSON o INSERT de la fila seleccionada o de la página, con las columnas visibles. CSV neutraliza fórmulas; INSERT usa el entrecomillado del motor (no disponible en MongoDB). Límite de 8 MB con aviso.
- **Pegar desde hoja de cálculo:** lee el formato de Excel (comillas, tabuladores, saltos de línea), valida cada celda con las reglas de edición y, si todo es válido, muestra una vista previa y añade filas nuevas pendientes de guardar y deshacibles. Si hay errores no añade nada y lista los primeros. Máx. 2000 filas y 4 MB.
- **Filtros guardados:** guarda con nombre los filtros escritos en las cajas, por conexión y tabla. Se almacena el estado de las cajas, no SQL; al aplicar se revalida contra las columnas actuales y se avisa de las condiciones omitidas.

## Verificación

- 115 pruebas unitarias, 0 fallidas; 9 autotests visuales (incluye columnas, portapapeles y filtros guardados), solo con SQLite.
- No verificado: fijar la primera columna (no implementado), persistencia tras reiniciar la app, edición con columnas reordenadas, confirmar un pegado y guardarlo en servidor, pegar desde Excel real, y todo contra servidores reales. El instalador sigue sin firma Authenticode.

---
# Db Pro 0.2.6

## Mejoras

- **Historial de consultas SQL:** botón «Historial» en el editor con búsqueda, favoritas, borrado de entradas y «Borrar historial» (conserva las favoritas). Solo se guarda el texto de las consultas ejecutadas con éxito, en este equipo (hasta 200 normales y 200 favoritas). Se omiten las consultas con errores y las que mencionan contraseñas o tokens (filtro conservador, también omite consultas inocentes como `select * from tokens`).
- **Ejecutar selección:** con texto seleccionado, F5, Ctrl+Intro y «Ejecutar» lanzan solo esa parte. Como Slint no expone la selección, se copia al portapapeles y se restaura su contenido anterior; si el portapapeles está ocupado, se ejecuta todo el texto.
- **Pestañas de consulta:** varias consultas abiertas, cada una con su texto y su resultado. Una consulta en curso termina en su pestaña aunque cambies de pestaña, y esa pestaña no se puede cerrar hasta que acabe o se cancele.

## Verificación

- 96 pruebas unitarias, 0 fallidas; 6 autotests visuales (core, persistencia de ajustes, error de conexión, historial, selección y pestañas), solo con SQLite.
- No verificado: autocompletado (no implementado: Slint 1.19 no expone la posición del cursor), borrado y filtros del historial desde la interfaz, atajos de teclado de la selección, y todo contra servidores reales. El instalador sigue sin firma Authenticode.

---
# Db Pro 0.2.5

## Mejoras

- **Tiempo límite de consultas:** nuevo ajuste (sin límite, 30 s, 1, 5 o 10 min) para el editor SQL y la carga de páginas. Usa el mecanismo de cancelación de cada conector; solo está verificado con SQLite. En PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server no se ha probado contra servidor.
- **Reconexión automática:** tras perder la conexión reintenta a 1, 2, 4, 8 y 16 s. Conserva los cambios sin guardar y nunca repite un guardado interrumpido sin que lo pidas. Probada la lógica y la detección con SQLite; no contra caídas reales de servidor.
- **Actualizador:** el instalador descargado se borra si su SHA-256 no coincide, y la descarga solo se acepta desde las versiones oficiales por HTTPS.

## Verificación

- 88 pruebas unitarias, 0 fallidas; autotests visuales: core, persistencia de ajustes y error de conexión.
- Actualización aislada 0.2.3 → 0.2.4 con la app abierta: reapertura, datos de usuario intactos y desinstalación limpia.
- Pendiente: tiempo límite de conexión por perfil; pruebas reales con PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server. No se usó MariaDB/XAMPP. El instalador sigue sin firma Authenticode.

---

# Db Pro 0.2.4

## Mejoras

- **Rendimiento:** la tabla de procesos usa todo el ancho disponible y filas de 34 px para que estados y consultas sean más legibles.
- **Editor SQL:** botón «Copiar resultado», que copia encabezados y filas como TSV compatible con hojas de cálculo.
- **Cancelación:** una cancelación ya solicitada se comprueba antes de iniciar operaciones de lectura y streaming para todos los conectores implementados. SQLite tiene pruebas específicas; DuckDB se verificó con cancelación de una consulta larga.

## Verificación

- CTest y suite unitaria: 79 pruebas, 0 fallidas.
- Autotest visual: core, persistencia de ajustes y error de conexión.
- DuckDB 1.5.6: 91/91 integración local, incluida cancelación.
- No se usó MariaDB/XAMPP. PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server no se volvieron a verificar contra servidor en esta publicación; SQL Server sigue solo compilado.

## Descargas

`DbPro-Setup-0.2.4.exe` y `DbPro-Portable-0.2.4.zip`. El instalador sigue sin firma Authenticode por no haber certificado disponible.

Código y artefactos: https://github.com/GonzaloCN-Dev/db-pro/releases/tag/v0.2.4

---

# Db Pro 0.2.3

El crédito de autoría ahora queda visible en todo momento en la barra inferior y más destacado en «Acerca de»: **Derechos de autor © 2026 Gonzalo Cruz Núñez**.

Instalador sin firma Authenticode (no había certificado disponible). Incluye el runtime de Visual C++.

Descargas: `DbPro-Setup-0.2.3.exe` y `DbPro-Portable-0.2.3.zip`.

Código y artefactos: https://github.com/GonzaloCN-Dev/db-pro/releases/tag/v0.2.3

---

# Db Pro 0.2.2

Mejoras del monitor y del flujo de pruebas, actividad local para SQLite/DuckDB, instalador con estimación de espacio y ZIP portable.

## Cambios

- Monitor: búsqueda local de sesiones, filtro de bloqueadas, orden ascendente/descendente y mensajes orientativos cuando los permisos impiden consultar o terminar sesiones.
- SQLite: métricas reales de conexión (memoria de caché/esquema/consulta y contadores de caché); sin lista de sesiones de servidor.
- DuckDB: memoria actual del motor y almacenamiento temporal vía `duckdb_memory()`; sin lista de sesiones ni terminación.
- Autotest visual con `assert`/`wait-for` y escenarios aislados para conexiones, errores, pestañas con cambios, ajustes, Acerca de y actividad local.
- Instalador: Inno calcula el tamaño de los componentes elegidos y reserva 50 MiB adicionales; el paquete completo actual requiere planificar aproximadamente 111 MiB. Actualización silenciosa 0.2.0 → 0.2.2 probada en un directorio temporal.
- Portable: `DbPro-Portable-0.2.2.zip`, almacena perfiles y ajustes dentro de `data`. Las contraseñas siguen vinculadas al Administrador de credenciales del usuario de Windows; no son portables.
- Firma digital: el script admite Authenticode si se proporciona un certificado válido. No había certificado disponible en el entorno; esta compilación permanece sin firmar.

## Verificación y límites

- Build MSVC y CTest: 76 tests, 0 fallidos.
- DuckDB 1.5.6: 91/91 comprobaciones, incluidas las métricas locales.
- Autotest UI: escenarios `core` y `connection-error` pasan; solo usan SQLite temporal.
- No se pudo probar la vista de sesiones/orden/filtro ni permisos con un servidor de PostgreSQL/MySQL/MongoDB/SQL Server en esta ejecución.

## Descargas

Instalador: `DbPro-Setup-0.2.2.exe`. Portable: `DbPro-Portable-0.2.2.zip`.
El instalador no está firmado; Windows SmartScreen puede mostrar un aviso de editor desconocido.

Código y artefactos: https://github.com/GonzaloCN-Dev/db-pro/releases/tag/v0.2.2

---

# Db Pro 0.2.1

Versión de mantenimiento centrada en estabilidad, diagnóstico local y atribución del producto.

## Cambios

- Se evita liberar el controlador mientras las tareas de conectores siguen activas al cerrar. Si un driver se queda bloqueado, la salida puede demorarse; los tiempos de cancelación deben validarse por driver.
- Registro local de cierres con código de excepción, módulo y fase. No guarda SQL, datos, nombre del servidor ni credenciales. Se abre desde Ajustes → Registros de cierres.
- «Acerca de» muestra **© 2026 Gonzalo Cruz Núñez**, la versión y un enlace a `https://github.com/GonzaloCN-Dev`.
- Ajuste persistente del tamaño de página (50–1000 filas).
- Guardar o descartar cambios pendientes se ofrece explícitamente al cerrar pestañas o abandonar una conexión.
- Build MSVC y 71 tests unitarios pasan. La reproducción de apertura en SQLite temporal no cerró la app.

## Límite de diagnóstico

El cierre reportado al abrir una tabla de SQL Server remoto no se pudo reproducir en el entorno disponible y no se considera resuelto. El conector SQL Server sigue sin integración probada contra un servidor real. Si vuelve a ocurrir, comparte el archivo `crashes.log` de la carpeta de datos (sin datos de conexión).

## Instalación

Descarga `DbPro-Setup-0.2.1.exe`. El instalador no está firmado; SmartScreen puede mostrar un aviso de editor desconocido.

Código y artefactos: https://github.com/GonzaloCN-Dev/db-pro/releases/tag/v0.2.1

---

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
