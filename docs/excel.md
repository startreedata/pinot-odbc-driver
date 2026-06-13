# Using Apache Pinot with Excel

Excel for Windows can pull Pinot data through its Power Query ODBC connector
— useful for ad-hoc analysis and refreshable workbook tables.

> Excel for Mac's ODBC support is limited to a few built-in sources; this
> guide covers **Excel for Windows** (Microsoft 365 / 2016+).

## 1. Install the driver and create a DSN

Follow steps 1–2 of the [Power BI guide](powerbi.md) — the same driver
registration and DSN serve every ODBC application on the machine:

```powershell
.\install_windows.ps1 -DriverPath C:\pinot-odbc\pinot_odbc.dll          # elevated
.\create_dsn_windows.ps1 -DsnName Pinot -BrokerHost broker.example.com -BrokerPort 8099 `
    -Controller controller.example.com:9000
```

## 2. Import data

1. **Data** tab → **Get Data** → **From Other Sources** → **From ODBC**.
2. Pick the `Pinot` DSN. To pin the exact query, expand **Advanced options**
   and fill **SQL statement (optional)**:

   ```sql
   SELECT playerName, SUM(homeRuns) AS homeRuns
   FROM baseballStats
   GROUP BY playerName
   ORDER BY homeRuns DESC
   LIMIT 500
   ```
3. Credentials dialog: **Default or Custom** (leave empty) for open clusters
   or token-in-DSN setups; **Database** for HTTP Basic username/password.
4. Without a SQL statement, the **Navigator** lists all Pinot tables — pick
   one, then **Load** (straight to a worksheet table) or **Transform Data**
   (Power Query editor first).

## 3. Refresh

The imported table remembers its query. **Data → Refresh All** (or
right-click the table → Refresh) re-runs it against Pinot. Auto-refresh on
file open: table **Query Properties** → *Refresh data when opening the file*.

## Tips

- Excel worksheets cap at ~1M rows and get slow well before that — aggregate
  in Pinot via the SQL statement instead of importing raw rows.
- Power Query transformations run client-side after import; put filters in
  the SQL statement, not in Power Query, to keep imports small.
- For JOINs across Pinot tables add `-UseMultistage` to the DSN (or
  `USEMULTISTAGE=1` in a connection string) and write the JOIN in the SQL
  statement.
- The troubleshooting table in the [Power BI guide](powerbi.md#troubleshooting)
  applies to Excel unchanged — same driver, same connector stack.
