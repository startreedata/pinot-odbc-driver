# Using Apache Pinot with Tableau

Tableau Desktop connects to Pinot through its generic ODBC connector
(**Other Databases (ODBC)**).

> StarTree also maintains a JDBC-based
> [Tableau connector](https://github.com/startreedata/startree-tableau-connector)
> (`.taco`) with Pinot-tuned SQL dialect support. Prefer it for production
> dashboards; the ODBC route below is handy when you want one driver shared
> with Excel/Power BI or can't install a `.taco` file.

## 1. Install the driver and create a DSN

Windows: follow steps 1–2 of the [Power BI guide](powerbi.md).
macOS: install the driver dylib and define the DSN in `odbcinst.ini` /
`odbc.ini` (see the [README](../README.md#installing-the-driver)) — Tableau
Desktop for Mac uses iODBC, which this driver supports.

## 2. Connect

1. **Connect** pane → **To a Server** → **More…** → **Other Databases (ODBC)**.
2. Select the `Pinot` **DSN** (or pick the driver and supply a connection
   string), then **Sign In**.
3. In the data source page, leave **Database**/**Schema** blank and click
   the table search icon — Tableau lists Pinot tables through the driver's
   catalog API. Drag a table onto the canvas, or use **New Custom SQL** to
   control the exact query (recommended).

## 3. Live vs. Extract

- **Extract** (recommended): Tableau pulls the data once and works against
  its local engine. Use Custom SQL with aggregation/`LIMIT` to keep extracts
  lean, and schedule refreshes as needed.
- **Live**: every worksheet interaction issues SQL through the driver.
  Pinot answers aggregations fast, but Tableau's generic-ODBC dialect probes
  can generate SQL that Pinot rejects; if you hit errors, switch to Custom
  SQL + Extract.

## Tips

- JOINs across Pinot tables need the multi-stage engine: add
  `-UseMultistage` to the DSN and express the JOIN in Custom SQL. For
  Tableau-side relationships between extracted tables, no engine change is
  needed.
- Tableau probes many `SQLGetInfo` capabilities at connect time; this driver
  reports a conservative SQL-92 entry-level dialect, so Tableau falls back
  to safe SQL generation.
- Long queries: bump `TIMEOUT` (HTTP seconds) and
  `QUERYOPTIONS=timeoutMs=...` in the DSN.
- Tableau Server / Cloud refreshes need the driver installed on the machine
  running the refresh (Server host or Bridge client).
