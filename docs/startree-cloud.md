# Connecting to StarTree Cloud

StarTree Cloud exposes the standard Pinot broker and controller APIs over
HTTPS with token authentication, so the ODBC driver connects to it the same
way as to any secured Pinot cluster.

## What you need

From your StarTree Cloud environment:

1. **Broker URL** — shown on the cluster's connection/query page, e.g.
   `https://broker.pinot.<env>.<org>.startree.cloud:443`.
2. **Controller URL** — e.g. `https://pinot.<env>.<org>.startree.cloud:443`
   (used for table/schema discovery: `SQLTables`, `SQLColumns`).
3. **An API token** for a service account or your user.

## Connection string

Use the full URLs with `BROKER=`/`CONTROLLER=` (the driver accepts either
`host:port` pairs or complete URLs):

```
DRIVER=PinotODBC;BROKER=https://broker.pinot.myenv.myorg.startree.cloud;CONTROLLER=https://pinot.myenv.myorg.startree.cloud;TOKEN=<api-token>
```

Token handling: the driver sends `Authorization: Bearer <token>` by default.
If your token already includes an auth scheme (e.g. a Basic credential), pass
it with the scheme and wrap it in braces so the semicolon-free value parses
cleanly:

```
TOKEN={Basic c2VydmljZS1hY2NvdW50OnNlY3JldA==}
```

DSN equivalents (odbc.ini on macOS/Linux, `create_dsn_windows.ps1` on
Windows):

```ini
[StarTreeCloud]
Driver     = PinotODBC
Broker     = https://broker.pinot.myenv.myorg.startree.cloud
Controller = https://pinot.myenv.myorg.startree.cloud
Token      = <api-token>
```

```powershell
.\create_dsn_windows.ps1 -DsnName StarTreeCloud `
    -BrokerHost broker.pinot.myenv.myorg.startree.cloud -BrokerPort 443 -Scheme https `
    -Controller pinot.myenv.myorg.startree.cloud:443 -Token <api-token>
```

## Multiple databases

If your environment uses Pinot logical databases, add `DATABASE=<name>` —
the driver sends it as the `database` header on every broker and controller
request.

## Notes

- TLS certificates on StarTree Cloud are publicly trusted; leave
  `SSLVERIFY` at its default (`1`).
- Query timeouts: `TIMEOUT=120` (HTTP) plus
  `QUERYOPTIONS=timeoutMs=120000` (Pinot-side) for heavy queries.
- The same connection string works in
  [Power BI](powerbi.md), [Excel](excel.md), [Tableau](tableau.md), and
  [Python](python.md).
