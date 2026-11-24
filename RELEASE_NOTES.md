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
