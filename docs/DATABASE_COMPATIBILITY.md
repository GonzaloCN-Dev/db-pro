# Compatibilidad de motores

Fuente de verdad del código: `src/core/engines.cpp`. Este documento recoge también lo pedido que **aún no existe**.
Estados: **Probado** (integración contra un servidor real de ese motor), **Experimental** (protocolo compatible o sin probar), **Planificado** (sin código todavía), **Excluido**.

## En la aplicación hoy (0.2.0 en desarrollo)

| Motor | Vía | Estado |
|---|---|---|
| SQLite | integrado | Probado |
| PostgreSQL | driver `postgres` | Probado (17.5) |
| MariaDB | driver `mysql` | Probado (10.4) |
| MySQL | driver `mysql` | Experimental |
| SQL Server | driver `mssql` (ODBC 18) | Experimental |
| MongoDB | driver `mongodb` | Probado (8.0 standalone) |
| Cloudberry, CockroachDB, CrateDB, EDB, Fujitsu Enterprise Postgres, GaussDB, Greengage, Greenplum, GreptimeDB, Kingbase, Materialize, RisingWave, TimescaleDB, Yellowbrick, YugabyteDB (YSQL) | driver `postgres` | Experimental |
| Databend, Apache Doris, MySQL NDB Cluster, OceanBase (modo MySQL), SingleStore, StarRocks, TiDB, VeloDB | driver `mysql` | Experimental |
| Azure SQL Database, Babelfish | driver `mssql` | Experimental |
| Amazon DocumentDB, FerretDB | driver `mongodb` | Experimental |

## Planificado por vía

**B. Driver nuevo, protocolo abierto (0.2.0 → siguientes):** DuckDB, SQLite cifrado (SQLCipher), libSQL, Firebird, DBF, Redis, Valkey, etcd, ClickHouse, Elasticsearch, OpenSearch, Open Distro, Apache Solr, CouchDB, Couchbase, InfluxDB 1.x/2.x/3.x, TDengine, Apache IoTDB, Trino, PrestoDB, PrestoSQL, Apache Avatica, Apache Kylin, Neo4j, Amazon Neptune, Cassandra, ScyllaDB, Amazon Keyspaces, Yugabyte CQL, DynamoDB, Firestore, Bigtable, Cosmos DB, Apache Ignite, Virtuoso, OrientDB.

**C. Driver ODBC genérico + dialecto (0.3.0):** Oracle, Db2 (LUW, z/OS, iSeries), Informix, SAP HANA, SAP MaxDB, SAP ASE, Sybase, Teradata, Netezza, Vertica, Exasol, Snowflake, Databricks, Cloudera, Microsoft Access, Altibase, CUBRID, Ingres, Mimer SQL, MonetDB, NuoDB, OpenEdge, Pervasive, Dameng, GBase 8s, InterSystems Caché e IRIS, Raima, SQream DB, OmniSci/HeavyDB, Ocient, Apache Hive, Spark, Impala, Drill, Phoenix, Kyuubi, Dremio, Arrow Flight (GizmoSQL), ksqlDB, TimechoDB, Machbase, DolphinDB, Timeplus, TDengine Cloud.
Los drivers ODBC del fabricante los instala el usuario; muchos son de pago y no redistribuibles.

## Excluidos
H2 y H2GIS (embebido), HSQLDB (embebido), Apache Derby (embebido), GemFire XD y SnappyData: solo Java. El servidor H2 se alcanza con su modo PostgreSQL. Se reconsiderará solo si se cambia la regla de no usar JVM (D-007).
