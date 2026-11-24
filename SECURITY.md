# Política de seguridad

## Informar de una vulnerabilidad

No informes de vulnerabilidades de seguridad en issues públicos. Usa el sistema de informes privados de vulnerabilidades de GitHub en este repositorio. Si todavía no está activado, contacta con el mantenedor a través de GitHub y pide un canal privado.

No publiques en un issue o discusión contraseñas, cadenas de conexión, tokens, volcados de bases de datos ni datos personales. Oculta rutas, nombres de servidor y datos de cuentas en los registros y capturas.

## Credenciales y datos

- Las contraseñas se guardan en el Administrador de credenciales de Windows (entradas con el prefijo `DbPro/`). Nunca se escriben en `connections.json`, en los registros ni en los perfiles exportados.
- Las conexiones y los ajustes se guardan en `%APPDATA%\DbPro`. Mantén esa carpeta privada: contiene nombres de servidor y usuarios.
- La validación del certificado TLS está activada por defecto. «Confiar en el certificado sin validarlo» es una opción explícita por conexión.
- Db Pro no envía telemetría.

## Drivers opcionales

Los drivers son DLL que se cargan en el proceso, así que se tratan como código ejecutable:

- Solo se descargan cuando pulsas «Descargar e instalar driver».
- La descarga usa HTTPS con validación de certificado, nunca sigue redirecciones a HTTP, tiene un tope de tamaño y se verifica con el SHA-256 del `drivers.json` de la versión antes de guardarse.
- Una DLL en `%APPDATA%\DbPro\drivers` solo se carga si su SHA-256 coincide con el que Db Pro registró al instalarla. Una DLL colocada por otra persona se rechaza.
- Un driver se rechaza si su número de ABI o la versión de la aplicación no coinciden con las de Db Pro.

## Operaciones que modifican datos

Db Pro nunca ejecuta scripts de DDL ni de sincronización automáticamente. Antes de guardar cambios muestra el SQL exacto; los cambios se aplican en una transacción y un conflicto de edición cancela todo el lote. Para trabajar con entornos delicados usa el modo de solo lectura y la etiqueta PRODUCCIÓN.
