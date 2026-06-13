# Power BI DirectQuery with the Pinot custom connector

Power BI's built-in ODBC connector only supports **Import** mode (it copies data
into the model). To query Pinot **live** — DirectQuery, where Power BI folds
each visual's filters and aggregations into Pinot SQL at interaction time — use
the **Apache Pinot custom connector** (`.mez`).

For Import mode without a custom connector, see [powerbi.md](powerbi.md).

## When to use DirectQuery vs. Import

| | Import | DirectQuery (this connector) |
|---|--------|------------------------------|
| Data freshness | As of last refresh | Live, every interaction |
| Dataset size | Must fit in the model | Stays in Pinot |
| Best for | Small/medium, pre-aggregated | Large tables, real-time dashboards |

DirectQuery is the natural fit for Pinot: visuals push `GROUP BY`/filters/`LIMIT`
down to the broker instead of pulling rows into Power BI.

## 1. Install the ODBC driver

The connector calls the Pinot ODBC driver, so install and register it first
(elevated PowerShell — see [powerbi.md](powerbi.md#1-install-the-driver)):

```powershell
.\install_windows.ps1 -DriverPath C:\pinot-odbc\pinot_odbc.dll
```

You do **not** need to create a DSN — the connector supplies the connection
details itself.

## 2. Install the connector

1. Download `pinot-odbc-powerbi-connector-<version>.mez` from the
   [releases](https://github.com/startreedata/pinot-odbc-driver/releases).
2. Copy it to `Documents\Power BI Desktop\Custom Connectors`
   (create the folder if it doesn't exist).
3. The connector is unsigned, so allow it in Power BI Desktop:
   **File → Options and settings → Options → Security → Data Extensions →**
   *(Not Recommended) Allow any extension to load without validation or warning*,
   then restart Power BI Desktop.

   (Alternatively, sign the `.mez` with your organization's certificate — see
   the [connector README](../powerbi-connector/README.md#signing).)

## 3. Connect

1. **Get Data** → search **Apache Pinot** → **Connect**.
2. Enter:
   - **host** — broker host (e.g. `broker.example.com`)
   - **port** — broker port (e.g. `8099`)
   - Expand **Advanced options** for `Scheme` (http/https), `Controller`
     (host:port, for table discovery), `Database`, and `UseMultistage`.
3. **Data Connectivity mode**: choose **DirectQuery** (or Import).
4. Credentials:
   - **Anonymous** — open clusters / quickstart
   - **Key** — a bearer **API token** (StarTree Cloud, secured clusters)
   - **Basic** — username/password (HTTP Basic)
5. In the **Navigator**, pick tables and build your report. With DirectQuery,
   each visual issues folded SQL to Pinot.

## Connecting to StarTree Cloud

Use `https`, the broker/controller hosts from the cluster's Connect page on
port 443, and the **Key** credential with your API token. See
[startree-cloud.md](startree-cloud.md).

## Query folding notes

- The connector declares Pinot-appropriate capabilities: `LIMIT n` paging,
  `CAST`, and SQL-92-entry-level dialect, so common filters, `GROUP BY`, and
  aggregates fold to Pinot.
- To see the generated SQL in Power Query: right-click a step → **View Native
  Query** (enabled when the step folds).
- Transformations Pinot can't express won't fold; Power BI then evaluates them
  locally (and DirectQuery may refuse some). Keep DirectQuery models close to
  what Pinot can answer — filters, group-bys, top-N.
- **JOINs** require Pinot's multi-stage engine: set **UseMultistage = true** in
  Advanced options. Single-table aggregations don't need it.

## Limitations

- DirectQuery support is **Beta** in this connector.
- The driver is read-only (Pinot is analytical; no DML/DDL).
- SQL conformance is declared conservatively (entry level) to avoid generating
  SQL Pinot rejects. If you need more folding and your cluster handles it,
  raise `Config_SqlConformance` and rebuild (see the connector README).

## Refresh through a gateway

For reports published to the Power BI Service, install the ODBC driver **and**
the `.mez` connector on the
[on-premises data gateway](https://learn.microsoft.com/power-bi/connect-data/service-gateway-onprem)
machine, and allow custom connectors in the gateway settings.

## Troubleshooting

Most issues match the [Import-mode troubleshooting table](powerbi.md#troubleshooting)
(driver registration, `08001` health-check, `28000` auth, empty Navigator →
controller unreachable). Connector-specific:

| Symptom | Fix |
|---------|-----|
| **Apache Pinot** not in Get Data | `.mez` not in `Documents\Power BI Desktop\Custom Connectors`, or the security setting wasn't lowered / connector not signed. Restart Power BI Desktop after both. |
| "*… requires evaluation outside DirectQuery*" | A step didn't fold. Simplify it or push the logic into Pinot; check **View Native Query** to see what folds. |
| JOIN errors in DirectQuery | Enable **UseMultistage = true**. |
