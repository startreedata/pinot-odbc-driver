# Using Apache Pinot with Power BI

This guide walks through connecting **Power BI Desktop** to Apache Pinot with
the Pinot ODBC driver, using Power BI's built-in ODBC connector in **Import
mode**.

> **Want live queries (DirectQuery)?** The built-in ODBC connector is
> Import-only. For DirectQuery, use the custom Apache Pinot connector — see
> [powerbi-directquery.md](powerbi-directquery.md).

## Prerequisites

- Windows with [Power BI Desktop](https://powerbi.microsoft.com/desktop/)
  (64-bit)
- Network access from your machine to the Pinot **broker** (queries) and
  **controller** (table discovery)
- The Pinot ODBC driver:
  [`pinot-odbc-driver-<version>-windows-x64.zip`](https://github.com/startreedata/pinot-odbc-driver/releases)

## 1. Install the driver

Unzip the release archive, then register the driver from an **elevated**
PowerShell prompt:

```powershell
Expand-Archive pinot-odbc-driver-<version>-windows-x64.zip -DestinationPath C:\pinot-odbc
cd C:\pinot-odbc
.\install_windows.ps1 -DriverPath C:\pinot-odbc\pinot_odbc.dll
```

Verify the registration:

```powershell
Get-OdbcDriver -Name "StarTree Pinot ODBC Driver"
```

## 2. Create a DSN (recommended)

A DSN keeps the connection details out of your reports. The driver does not
ship a GUI configuration dialog, so create the DSN with the helper script
(no admin rights needed — it creates a per-user DSN):

```powershell
.\create_dsn_windows.ps1 -DsnName Pinot -BrokerHost broker.example.com -BrokerPort 8099 `
    -Controller controller.example.com:9000
```

Useful optional parameters:

| Parameter | Effect |
|-----------|--------|
| `-Scheme https` | TLS to broker/controller |
| `-Token <token>` | Bearer-token auth (StarTree Cloud and secured clusters) |
| `-Database <db>` | Pinot database header |
| `-UseMultistage` | Run queries on the multi-stage engine (needed for JOINs) |
| `-QueryOptions "timeoutMs=60000"` | Raw Pinot query options |
| `-SslVerify 0` | Skip TLS certificate verification (self-signed certs) |

Alternatively, skip the DSN and use a raw connection string in Power BI (see
step 3).

## 3. Connect from Power BI Desktop

1. **Get Data** → **More…** → **Other** → **ODBC** → **Connect**.
2. Either:
   - pick your DSN (e.g. `Pinot`) from the **Data source name (DSN)**
     dropdown, **or**
   - select **(None)**, expand **Advanced options**, and paste a connection
     string:

     ```
     DRIVER={StarTree Pinot ODBC Driver};HOST=broker.example.com;PORT=8099;CONTROLLER=controller.example.com:9000
     ```
3. (Optional, recommended for large tables) In **Advanced options** →
   **SQL statement**, enter the exact query to import, e.g.:

   ```sql
   SELECT playerName, SUM(homeRuns) AS homeRuns
   FROM baseballStats
   GROUP BY playerName
   ORDER BY homeRuns DESC
   LIMIT 1000
   ```
4. In the credential dialog:
   - **Default or Custom** for unauthenticated clusters or when the token is
     already in the DSN/connection string;
   - **Database** to enter a username/password — Power BI passes them to the
     driver as `UID`/`PWD` (HTTP Basic auth).
5. Without a SQL statement, the **Navigator** lists every Pinot table
   (discovered through the controller). Tick the tables you need, then
   **Load** or **Transform Data**.

## Authentication options

| Cluster setup | What to use |
|---------------|-------------|
| No auth | Credential dialog → *Default or Custom*, leave empty |
| HTTP Basic | Credential dialog → *Database* (username/password), or `UID=...;PWD=...` in the DSN/connection string |
| Bearer token | `TOKEN=<token>` in the DSN/connection string |
| StarTree Cloud | `SCHEME=https`, broker/controller URLs from the cluster's *Connect* page, plus `TOKEN=...` |

## Tips for large tables

- **Import mode buffers the full broker response.** Always constrain what you
  import: use the **SQL statement** box with `WHERE`/`LIMIT`, or do the
  aggregation in Pinot (`GROUP BY` in the SQL statement) instead of importing
  raw rows.
- Power Query transformations applied after load do **not** fold back into
  Pinot SQL — push the heavy lifting into the SQL statement.
- Pinot queries also respect the broker's own row/timeout limits; raise them
  per query with `QUERYOPTIONS` (e.g. `timeoutMs=60000`) if needed.
- For JOINs across Pinot tables, add `USEMULTISTAGE=1` to the DSN/connection
  string and write the JOIN in the SQL statement.

## Refreshing published reports

Reports published to the Power BI Service refresh through an
[on-premises data gateway](https://learn.microsoft.com/power-bi/connect-data/service-gateway-onprem).
Install and register the driver (steps 1–2) **on the gateway machine**, and
create the same DSN there if the report uses one.

## Limitations

- **Import mode only (this guide).** Power BI's *generic* ODBC connector does
  not support DirectQuery. For live DirectQuery, use the custom Apache Pinot
  connector instead — see [powerbi-directquery.md](powerbi-directquery.md).
- The driver is read-only — Pinot is an analytical store; no DML/DDL.
- ODBC escape sequences (`{ts '...'}`, `{fn ...}`) are passed through to
  Pinot verbatim; write plain Pinot SQL in the SQL statement box.

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| *Data source name not found and no default driver specified* | Driver not registered, or registered for the wrong bitness. Re-run `install_windows.ps1` from elevated PowerShell; confirm with `Get-OdbcDriver`. Power BI Desktop is 64-bit — manage DSNs with the 64-bit *ODBC Data Sources* app. |
| DSN missing from the dropdown | The helper script creates a per-user DSN; make sure Power BI runs as the same Windows user. |
| *08001: Pinot broker health check failed* | Wrong host/port/scheme, or the broker is unreachable from your machine. Test with `curl http://<broker>:<port>/health`. Add `HEALTHCHECK=0` to skip the probe if your deployment blocks `/health`. |
| *28000: Authentication failed* | Bad/missing credentials. For bearer tokens use `TOKEN=...`; for basic auth use the *Database* credential type or `UID`/`PWD`. |
| TLS errors against self-signed certificates | Add `SSLVERIFY=0` (testing only) or install the CA certificate. |
| Navigator is empty | The driver lists tables via the **controller** — verify the `CONTROLLER=` host:port is set and reachable. |
| Import is slow / runs out of memory | Use the SQL statement box with `LIMIT`/aggregations instead of importing whole tables. |
