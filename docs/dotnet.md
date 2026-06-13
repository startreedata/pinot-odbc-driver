# Using Apache Pinot from .NET (C#)

[`System.Data.Odbc`](https://learn.microsoft.com/dotnet/api/system.data.odbc)
provides the standard ADO.NET interface over ODBC on Windows, macOS, and
Linux.

## Setup

Install the driver and register it (see the
[README](../README.md#installing-the-driver)), then add the package:

```sh
dotnet add package System.Data.Odbc
```

On macOS/Linux, `System.Data.Odbc` loads the unixODBC driver manager
(`libodbc`); on Windows it uses the built-in one.

## Connecting and querying

```csharp
using System.Data.Odbc;

var connectionString =
    "DRIVER={StarTree Pinot ODBC Driver};" +   // POSIX: DRIVER=PinotODBC
    "HOST=localhost;PORT=8099;CONTROLLER=localhost:9000";
// or: "DSN=Pinot"

using var conn = new OdbcConnection(connectionString);
conn.Open();

using var cmd = new OdbcCommand(
    "SELECT playerName, homeRuns FROM baseballStats ORDER BY homeRuns DESC LIMIT 5", conn);
using var reader = cmd.ExecuteReader();
while (reader.Read())
{
    Console.WriteLine($"{reader.GetString(0)}: {reader.GetInt32(1)}");
}
```

Parameterized queries use positional `?` markers (substituted client-side as
escaped literals):

```csharp
using var cmd = new OdbcCommand(
    "SELECT count(*) FROM baseballStats WHERE yearID >= ? AND league = ?", conn);
cmd.Parameters.AddWithValue("@year", 2000);
cmd.Parameters.AddWithValue("@league", "NL");
var count = Convert.ToInt64(cmd.ExecuteScalar());
```

Catalog discovery via ADO.NET schema collections:

```csharp
var tables = conn.GetSchema("Tables");
foreach (System.Data.DataRow row in tables.Rows)
{
    Console.WriteLine(row["TABLE_NAME"]);
}
var columns = conn.GetSchema("Columns", new[] { null, null, "baseballStats", null });
```

## Type mapping

| Pinot | .NET (`reader.GetX`) |
|-------|----------------------|
| `INT` | `GetInt32` |
| `LONG` | `GetInt64` |
| `FLOAT` / `DOUBLE` | `GetFloat` / `GetDouble` |
| `BIG_DECIMAL` | `GetDecimal` (or `GetString`) |
| `BOOLEAN` | `GetBoolean` |
| `TIMESTAMP` | `GetDateTime` |
| `STRING` / `JSON` / `*_ARRAY` | `GetString` |
| `BYTES` | `GetBytes` / `reader[i]` as `byte[]` |

Check `reader.IsDBNull(i)` before typed getters — Pinot columns are nullable.

## Notes

- Pinot has no transactions; leave the connection in its default
  auto-commit state and skip `BeginTransaction`.
- Keep result sets bounded (`LIMIT`, aggregations) — the driver buffers the
  broker's full JSON response.
- `CommandTimeout` maps to the driver's per-statement query timeout.
- JOINs need `USEMULTISTAGE=1` in the connection string.
