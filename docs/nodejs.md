# Using Apache Pinot from Node.js

The [`odbc`](https://www.npmjs.com/package/odbc) npm package gives Node.js a
promise-based API over the system driver manager.

## Setup

Install the driver and register it (see the
[README](../README.md#installing-the-driver)), then:

```sh
npm install odbc
```

The package compiles a native addon against unixODBC on macOS/Linux
(`brew install unixodbc` / `apt install unixodbc-dev`) and against the
built-in driver manager on Windows.

## Connecting and querying

```js
import odbc from 'odbc';

const conn = await odbc.connect(
  'DRIVER=PinotODBC;HOST=localhost;PORT=8099;CONTROLLER=localhost:9000');
// or: await odbc.connect('DSN=Pinot');

const rows = await conn.query(
  'SELECT playerName, homeRuns FROM baseballStats ORDER BY homeRuns DESC LIMIT 5');
for (const row of rows) {
  console.log(row.playerName, row.homeRuns);
}
console.log(rows.columns.map((c) => c.name));  // result metadata

// Parameterized queries ('?' markers are substituted client-side
// as escaped literals):
const since2000 = await conn.query(
  'SELECT count(*) FROM baseballStats WHERE yearID >= ? AND league = ?',
  [2000, 'NL']);

// Catalog discovery:
const tables = await conn.tables(null, null, null, null);
console.log(tables.map((t) => t.TABLE_NAME));

await conn.close();
```

For concurrent workloads use a pool:

```js
const pool = await odbc.pool('DSN=Pinot');
const result = await pool.query('SELECT count(*) FROM baseballStats');
await pool.close();
```

## Notes

- `BOOLEAN` columns arrive as `1`/`0`; `TIMESTAMP` as strings
  (`yyyy-MM-dd HH:mm:ss.fff`); multi-value `*_ARRAY` columns as JSON text —
  `JSON.parse` them.
- The driver buffers the broker's full response; keep result sets bounded
  with `LIMIT` or aggregations.
- JOINs need the multi-stage engine: add `USEMULTISTAGE=1` to the connection
  string.
- Auth: `UID`/`PWD` (HTTP Basic) or `TOKEN=...` (bearer) — see
  [StarTree Cloud](startree-cloud.md).
