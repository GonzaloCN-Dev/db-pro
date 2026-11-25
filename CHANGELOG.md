# Changelog

## 0.2.6
- Historial de consultas SQL con búsqueda y favoritas; solo guarda el texto de consultas correctas y omite las que mencionan contraseñas o tokens.
- Ejecutar solo el texto seleccionado (F5, Ctrl+Intro o Ejecutar).
- Pestañas de consulta con texto y resultado propios.
- 96 pruebas unitarias y 6 autotests visuales.
## 0.2.5
- Nuevo ajuste «Tiempo límite de consultas» (sin límite, 30 s, 1, 5 o 10 min) para el editor SQL y la carga de páginas. Al cumplirse se detiene la consulta y se avisa de que el servidor puede seguir terminando el trabajo.
- Reconexión automática con esperas crecientes (1, 2, 4, 8 y 16 s) tras perder la conexión. Los cambios sin guardar se conservan y un guardado interrumpido nunca se repite solo.
- La actualización verifica el SHA-256 y borra el instalador si no coincide; nuevas pruebas de descarga solo desde el repositorio oficial por HTTPS.
- Prueba aislada de actualización de extremo a extremo (`tests/updater/run-update-test.ps1`): 0.2.3 → 0.2.4 con la app abierta, reapertura, datos intactos y desinstalación limpia.
- 88 pruebas unitarias.

## 0.2.4
- La vista Rendimiento aprovecha todo el ancho de la ventana y sus filas de procesos son más altas para facilitar la lectura.
- El editor SQL permite copiar su resultado al portapapeles como TSV, además de exportarlo.
- Los tokens de cancelación ya solicitados se respetan antes de iniciar operaciones de lectura/streaming en SQLite, DuckDB, PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server.
- SQLite prueba que las operaciones precanceladas no devuelven ni modifican datos; DuckDB 1.5.6 pasa 91/91 pruebas de integración local, incluida cancelación real.

## 0.2.3
- El nombre del autor y los derechos de autor quedan visibles permanentemente en la barra inferior y destacados en «Acerca de»: «Derechos de autor © 2026 Gonzalo Cruz Núñez».

## 0.2.2
- Monitor con búsqueda de sesiones, filtro «solo bloqueadas», orden por columnas y mensajes orientativos para permisos de monitorización.
- Actividad local real para SQLite (memoria de caché y contadores) y DuckDB (memoria del motor/almacenamiento temporal); no muestra sesiones ni ofrece terminación para estos motores.
- Autotest UI con aserciones y espera por estado: perfiles temporales, pestañas sucias, ajustes, errores de conexión y monitor local.
- Instalador con reserva de espacio adicional y estimación documentada del espacio instalado; probada la actualización silenciosa 0.2.0 → 0.2.2 en una carpeta temporal.
- Paquete portable ZIP evaluado y probado en carpeta temporal. Los perfiles/ajustes son locales al paquete; contraseñas siguen en Windows Credential Manager y no viajan con él.
- Firma Authenticode condicional; no se encontró certificado de firma disponible en el entorno, por lo que el instalador se publica sin firmar.
- 76 tests unitarios pasan; DuckDB 91/91 en prueba de integración local.

## 0.2.1
- Evita destruir el controlador mientras quedan tareas de conectores en curso al cerrar la aplicación; la salida espera a que terminen.
- Diagnóstico local de cierres: código de excepción, módulo y fase, sin SQL, nombres de servidor ni credenciales; acceso a los registros desde Ajustes.
- Diálogo «Acerca de» con copyright y enlace al repositorio.
- Preferencia persistente de filas por página.
- Guardado/descartado explícito de cambios pendientes al cerrar pestañas o cambiar de conexión.
- 71 tests unitarios pasan. El cierre reportado con SQL Server remoto no se ha podido reproducir ni atribuir a una causa concreta.

## 0.2.0
- Ajustes persistentes para tema, idioma, actualizaciones opcionales e intervalo del monitor.
- Comprobación y descarga de actualizaciones desde GitHub Releases; verificación SHA-256 e instalación silenciosa con relanzamiento.
- Varias tablas en pestañas, con filtros, orden, página y cambios pendientes independientes.
- Vista de rendimiento en vivo para PostgreSQL, MySQL/MariaDB, MongoDB y SQL Server; muestra las capacidades disponibles y requiere confirmación para terminar sesiones.
- Driver DuckDB probado contra DuckDB 1.5.6 (88 comprobaciones).
- Suite 0.2.0: 67 tests unitarios. Las funciones nuevas de monitor y actualización aún no se han probado end-to-end contra servidores reales ni con una actualización publicada.

## 0.1.0
- Núcleo, interfaz Slint, cuadrícula de datos editable con filtros y ordenación en servidor, exportación en streaming.
- SQLite integrado. Drivers opcionales: MySQL/MariaDB, PostgreSQL, MongoDB (probados contra servidores reales desechables) y SQL Server (compilado, sin probar contra un servidor).
- Motores compatibles (YugabyteDB, TiDB, EDB, CrateDB, ...) como experimentales sobre los drivers existentes.
- Instalador con un componente por driver; drivers descargables con verificación SHA-256.
