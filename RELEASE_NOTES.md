# Db Pro 0.1.0

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
| MySQL / MariaDB | Experimental: aún no probado contra un servidor real |
| PostgreSQL, SQL Server, MongoDB | En desarrollo, aún no disponibles |

## Instalar

Descarga `DbPro-Setup-0.1.0.exe` y ejecútalo. El instalador no está firmado, así que Windows SmartScreen puede mostrar un aviso de editor desconocido.

## Notas

- Windows 10/11 de 64 bits.
- Interfaz en español e inglés.
- Licencia MIT para el código original de Db Pro; consulta `installer/LICENSES.txt` para los componentes de terceros.
- Creado por Gonzalo Cruz Núñez.
