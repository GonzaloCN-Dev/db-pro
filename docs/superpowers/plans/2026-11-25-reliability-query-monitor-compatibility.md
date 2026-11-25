# Fiabilidad, consultas, monitor y compatibilidad

**Objetivo:** mejorar los cuatro bloques solicitados sin presentar como probadas funciones que no se hayan ejecutado.

**Restricciones:** C++20 y Slint; acceso a bases a través de `Connector`; UI sin bloqueos; transacciones atómicas y concurrencia optimista; sin pruebas destructivas en servidores reales ni uso de XAMPP 3306; perfiles y contraseñas siguen protegidos. El copyright y el enlace del autor permanecen visibles.

## Fases y comprobaciones

- [ ] **Fiabilidad:** inventariar la cancelación y los tiempos de espera por driver, diseñar un ciclo de vida seguro y probar las rutas disponibles con bases temporales. Ampliar las pruebas de guardar, cancelar, conflicto y errores. Ejercitar el actualizador completo solo en un entorno aislado, sin afectar instalaciones ni perfiles reales.
- [ ] **Consultas:** diseñar almacenamiento acotado de historial y favoritos sin guardar credenciales accidentalmente; implementar autocompletado con metadatos reales, filtros guardados y mejoras de cuadrícula (copiar/pegar, ocultación y anchos), con pruebas de persistencia y casos límite.
- [ ] **Monitor:** dar prioridad al espacio de la tabla de procesos, preservar lectura y selección al cambiar tamaño/orden, añadir histórico acotado de métricas y comprobar bloqueo y errores de permisos sin inventar sesiones. Verificar vistas a distintos tamaños; validar con servidores reales únicamente cuando estén disponibles.
- [ ] **Compatibilidad:** ejecutar pruebas aisladas de autenticación y TLS donde existan servidores desechables autorizados; documentar por separado los motores y escenarios no disponibles, especialmente SQL Server. No usar la instancia XAMPP.
- [ ] **Release:** ejecutar compilación, CTest y autotests aplicables; revisar documentación, cambios y artefactos; publicar únicamente lo realmente implementado y verificado, con SHA-256 y aviso si sigue sin firma.

## Riesgos que deben probarse

- Cancelar una operación durante la desconexión no debe destruir un conector que siga en uso.
- Un timeout nunca debe presentarse como cancelación confirmada si el servidor sigue trabajando.
- Los filtros guardados se validan contra los metadatos actuales y no permiten inyectar SQL.
- El monitor debe manejar ventanas estrechas, procesos con texto largo y respuestas obsoletas sin perder la identidad de sesión.
- Ninguna prueba de instalación o actualización debe modificar datos, credenciales o instalaciones reales.
