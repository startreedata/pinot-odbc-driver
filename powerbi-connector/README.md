# Apache Pinot — Power BI custom connector

A Power BI custom connector (`.mez`) that wraps the
[Pinot ODBC driver](../README.md) with `Odbc.DataSource`, enabling **DirectQuery**
(live, folded queries) in addition to Import mode. Power BI's built-in generic
ODBC connector only supports Import; this connector is what unlocks DirectQuery.

End-user setup and usage is documented in
[docs/powerbi-directquery.md](../docs/powerbi-directquery.md). This file covers
building the connector.

## Contents

| File | Purpose |
|------|---------|
| `PinotODBC.pq` | The connector (M section document) |
| `PinotODBC.proj` | MSBuild project that zips the `.mez` |
| `PinotODBC.query.pq` | Test queries for the Power Query SDK |
| `OdbcConstants.pqm` | ODBC constant tables (vendored from microsoft/DataConnectors, MIT) |
| `PinotODBC*.png` | Connector icons (16–64 px) |

## What it configures

- **DirectQuery** enabled (`SupportsDirectQuery = true`).
- **Query folding** tuned for Pinot: `LimitClauseKind.Limit` (Pinot's `LIMIT n`),
  `CAST` instead of `CONVERT`, single-quote string escaping, and literal
  inlining (the driver substitutes parameters as escaped literals).
- **Authentication**: Anonymous, Key (bearer token → `TOKEN=`), and
  UsernamePassword (HTTP Basic → `UID`/`PWD`).
- **Options**: `Scheme`, `Controller`, `Database`, `UseMultistage`,
  `ConnectionTimeout`.

SQL conformance is declared at SQL-92 entry level (matching what the driver
reports) to keep folded SQL within what Pinot accepts. Raise
`Config_SqlConformance` in `PinotODBC.pq` if you want more aggressive folding
and your cluster handles the richer SQL.

## Building

The `.mez` is just a ZIP of the `.pq`, `.pqm`, and `.png` files. Build it any
of these ways:

```sh
# Cross-platform, no SDK required (this is what CI uses):
dotnet msbuild powerbi-connector/PinotODBC.proj -t:BuildMez
# -> powerbi-connector/bin/AnyCPU/Debug/PinotODBC.mez
```

Or with the [Power Query SDK](https://learn.microsoft.com/power-query/install-sdk)
in VS Code (**Run Build Task → Build connector project**), which also gives you
in-editor query testing via `PinotODBC.query.pq`.

CI ([.github/workflows/powerbi-connector.yml](../.github/workflows/powerbi-connector.yml))
builds the `.mez` and compile-validates the M with the Power Query SDK tools on
every change. Tagged releases attach a versioned `.mez` to the GitHub Release.

## Signing

The connector is unsigned. Users either lower Power BI's security to allow
"any extension" (documented in the user guide) or you sign it with your own
certificate:

```
MakePQX.exe pack -mz PinotODBC.mez -c cert.pfx -p <password> -t PinotODBC.pqx
```

See [Handling Connector Signing](https://learn.microsoft.com/power-query/handling-connector-signing).
