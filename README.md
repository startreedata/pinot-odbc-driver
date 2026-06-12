# Apache Pinot ODBC Connector

An ODBC 3.x driver for [Apache Pinot](https://pinot.apache.org/). It talks to
the Pinot **broker** REST API (`POST /query/sql`) for queries and to the
**controller** API (`GET /tables`, `GET /tables/{name}/schema`) for catalog
metadata, so any ODBC-capable tool (isql, pyodbc, Excel, BI tools via
unixODBC/iODBC) can query Pinot.

```
┌─────────────┐   ODBC API    ┌──────────────────┐   HTTP/JSON   ┌────────────────┐
│ Application │ ────────────► │ libpinot_odbc    │ ────────────► │ Pinot broker   │
│ (pyodbc,    │               │ (this driver)    │               │  /query/sql    │
│  isql, BI)  │               │                  │ ────────────► │ Pinot controller│
└─────────────┘               └──────────────────┘               │  /tables, /schema│
                                                                 └────────────────┘
```

## Features

- **Queries**: `SQLExecDirect`, `SQLPrepare`/`SQLExecute` with client-side
  `?` parameter substitution (`SQLBindParameter`, `SQLNumParams`)
- **Fetching**: `SQLFetch`/`SQLFetchScroll` (forward-only), `SQLBindCol`,
  chunked `SQLGetData`, full C-type conversion matrix (char, wide char,
  integers with range checking, float/double, bit, timestamp/date/time
  structs, binary)
- **Catalog**: `SQLTables`, `SQLColumns`, `SQLPrimaryKeys` (from the Pinot
  schema's `primaryKeyColumns`), `SQLGetTypeInfo`, plus well-formed empty
  result sets for statistics/foreign keys/procedures
- **Unicode**: full set of `W` entry points (UTF-16 ⇄ UTF-8), so Unicode
  applications such as pyodbc work without driver-manager conversion
- **Auth**: HTTP Basic (`UID`/`PWD`) or bearer/custom token (`TOKEN`),
  Pinot `database` header support
- **Engine options**: multi-stage query engine (`USEMULTISTAGE=1`), raw
  `queryOptions` pass-through, HTTPS with optional certificate verification

## Type mapping

| Pinot type    | SQL type            | Default C type          |
|---------------|---------------------|-------------------------|
| `INT`         | `SQL_INTEGER`       | `SQL_C_SLONG`           |
| `LONG`        | `SQL_BIGINT`        | `SQL_C_SBIGINT`         |
| `FLOAT`       | `SQL_REAL`          | `SQL_C_FLOAT`           |
| `DOUBLE`      | `SQL_DOUBLE`        | `SQL_C_DOUBLE`          |
| `BIG_DECIMAL` | `SQL_DECIMAL`       | `SQL_C_CHAR` (string)   |
| `BOOLEAN`     | `SQL_BIT`           | `SQL_C_BIT`             |
| `TIMESTAMP`   | `SQL_TYPE_TIMESTAMP`| `SQL_C_TYPE_TIMESTAMP`  |
| `STRING`      | `SQL_VARCHAR`       | `SQL_C_CHAR`            |
| `JSON`        | `SQL_LONGVARCHAR`   | `SQL_C_CHAR`            |
| `BYTES`       | `SQL_VARBINARY`     | `SQL_C_BINARY` (hex-decoded) |
| `*_ARRAY` (MV)| `SQL_VARCHAR`       | `SQL_C_CHAR` (JSON text, e.g. `[1,2,3]`) |

`TIMESTAMP` values are accepted both as `yyyy-MM-dd HH:mm:ss[.fff]` strings
and as epoch milliseconds.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, libcurl (system), and the
ODBC SDK headers from unixODBC.

```sh
# macOS
brew install unixodbc cmake
# Debian/Ubuntu
# apt-get install unixodbc-dev cmake g++ libcurl4-openssl-dev

cmake -S . -B build
cmake --build build
# -> build/libpinot_odbc.dylib (macOS) / build/libpinot_odbc.so (Linux)
```

Run the test suite (no Pinot cluster needed; an in-process mock broker is
used):

```sh
cd build && ctest --output-on-failure
```

### Live integration tests (Docker)

Like the other Pinot clients, the live tests run against the real Pinot batch
quickstart (`apachepinot/pinot` image, `QuickStart -type batch`) and verify
queries on the bundled `baseballStats` table — catalog discovery, counts,
selections, aggregations/group-by, parameterized queries, Unicode, and error
propagation:

```sh
# Starts the quickstart container, waits until baseballStats is queryable,
# runs build/live_integration_tests, and tears the container down.
test/run_docker_integration.sh
```

Overrides: `PINOT_IMAGE` (default `apachepinot/pinot:1.3.0`), `BROKER_PORT`
(8000), `CONTROLLER_PORT` (9000), `READY_TIMEOUT` (600s), `KEEP_CLUSTER=1` to
leave the container running. To register the test with ctest, configure with
`-DPINOT_ODBC_DOCKER_TESTS=ON` (labeled `docker`, 15 min timeout).

The binary can also target an existing cluster directly:

```sh
PINOT_BROKER=localhost:8000 PINOT_CONTROLLER=localhost:9000 build/live_integration_tests
```

## Installing the driver

Register the driver with your driver manager (`odbcinst -j` shows where the
config files live):

```ini
# odbcinst.ini
[PinotODBC]
Description = Apache Pinot ODBC Driver
Driver      = /usr/local/lib/libpinot_odbc.dylib
```

Define a DSN:

```ini
# odbc.ini
[Pinot]
Driver     = PinotODBC
Host       = localhost
Port       = 8099
Controller = localhost:9000
# UID      = user
# PWD      = password
# TOKEN    = my-bearer-token
# DATABASE = mydb
# USEMULTISTAGE = 1
```

Or skip the DSN and use a full connection string:

```
DRIVER=PinotODBC;HOST=broker.example.com;PORT=8099;SCHEME=https;CONTROLLER=controller.example.com:9000;TOKEN=...
```

### Connection string / DSN parameters

| Key | Default | Description |
|-----|---------|-------------|
| `HOST` / `SERVER` | `localhost` | Broker host |
| `PORT` | `8099` | Broker port |
| `SCHEME` | `http` | `http` or `https` |
| `BROKER` | — | Broker as `host:port` or a full URL (overrides `HOST`/`PORT`) |
| `CONTROLLER` | broker host, port 9000 | Controller as `host:port` or full URL; used by `SQLTables`/`SQLColumns`/`SQLPrimaryKeys` |
| `UID` / `PWD` | — | HTTP Basic auth credentials |
| `TOKEN` | — | `Authorization` header value; `Bearer ` is prepended unless the value already contains a scheme (e.g. `Basic dXNlcjpwYXNz`) |
| `DATABASE` | — | Pinot database name, sent as the `database` header |
| `TIMEOUT` | `60` | HTTP timeout in seconds |
| `SSLVERIFY` | `1` | Verify TLS certificates |
| `USEMULTISTAGE` | `0` | Add `useMultistageEngine=true` to queryOptions |
| `QUERYOPTIONS` | — | Raw Pinot queryOptions string, e.g. `timeoutMs=5000` |
| `STRINGCOLUMNSIZE` | `4096` | Column size reported for `STRING` columns |
| `HEALTHCHECK` | `1` | Probe broker `/health` during connect |

## Usage examples

### isql

```sh
echo "SELECT playerName, sum(homeRuns) FROM baseballStats GROUP BY playerName LIMIT 5" | isql -v Pinot
```

### pyodbc

```python
import pyodbc

conn = pyodbc.connect("DSN=Pinot", autocommit=True)
cur = conn.cursor()
cur.execute("SELECT * FROM airlineStats WHERE Carrier = ? LIMIT 10", "AA")
for row in cur.fetchall():
    print(row)

print([t.table_name for t in cur.tables()])           # catalog
print([c.column_name for c in cur.columns("airlineStats")])
```

See [examples/](examples/) for sample config files and a runnable script.

## Limitations

- **Read-only**: Pinot is an analytical store; DML/DDL and transactions are
  not supported (`SQL_TXN_CAPABLE = SQL_TC_NONE`, commit/rollback are no-ops;
  `SQL_ATTR_AUTOCOMMIT` is accepted for client compatibility).
- **Forward-only cursors**, row array size 1 (block fetching is negotiated
  down with `01S02`).
- **Client-side parameters**: `?` markers are substituted into the SQL text
  as escaped literals before the query is sent — Pinot's HTTP API has no
  server-side prepare. Data-at-execution (`SQLPutData`) is not supported.
- **Result set is fully buffered**: the broker returns the complete JSON
  response; use `LIMIT` or `SQL_ATTR_MAX_ROWS` for large results.
- Multi-value (array) columns are surfaced as JSON-encoded `VARCHAR`.
- `SQLDescribeParam` reports `VARCHAR` (no server-side type inference).

## Project layout

```
src/
  odbc_handles.cpp   handle lifecycle, env/dbc/stmt attributes, diagnostics
  odbc_connect.cpp   SQLConnect/SQLDriverConnect, SQLGetInfo, SQLGetFunctions
  odbc_stmt.cpp      execute/prepare/fetch/bind/describe/getdata
  odbc_catalog.cpp   SQLTables/SQLColumns/SQLPrimaryKeys/SQLGetTypeInfo
  odbc_unicode.cpp   W entry points (UTF-16/UTF-32 <-> UTF-8)
  pinot_client.*     libcurl HTTP client for broker + controller
  result_set.*       resultTable JSON -> column metadata + rows
  convert.*          cell -> C type conversion engine
  config.*           connection string / odbc.ini DSN parsing
  types.*            Pinot <-> SQL type mapping
test/
  unit_tests.cpp     parsing, type mapping, conversions, unicode
  integration_tests.cpp  full ODBC call flows against an in-process mock broker
  mock_broker.*      tiny HTTP server mimicking broker + controller
  mock_server_main.cpp   standalone mock for manual isql/pyodbc testing
```

## License

Licensed under the [StarTree Community License](LICENSE)
(<http://www.startree.ai/startree-community-license>).

The vendored [third_party/json.hpp](third_party/json.hpp)
([nlohmann/json](https://github.com/nlohmann/json)) is MIT-licensed; see the
notice embedded in that file.
