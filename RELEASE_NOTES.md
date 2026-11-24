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
