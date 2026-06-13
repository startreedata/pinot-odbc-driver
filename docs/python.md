# Using Apache Pinot from Python (pyodbc + pandas)

The driver works with any DBAPI-style ODBC bridge; this guide uses
[pyodbc](https://github.com/mkleehammer/pyodbc), the most common one.

## Setup

Install the driver and register it with your driver manager
(see the [README](../README.md#installing-the-driver)), then:

```sh
pip install pyodbc            # add pandas for DataFrame examples
```

On macOS/Linux, pyodbc links against unixODBC — the same driver manager the
driver is registered with (`brew install unixodbc` / `apt install unixodbc`).
On Windows, pyodbc uses the built-in driver manager; register the driver with
`examples/install_windows.ps1`.

## Connecting

```python
import pyodbc

# Via a DSN defined in odbc.ini (POSIX) or the registry (Windows):
conn = pyodbc.connect("DSN=Pinot", autocommit=True)

# Or fully self-contained:
conn = pyodbc.connect(
    "DRIVER=PinotODBC;"            # name from odbcinst.ini / registry
    "HOST=localhost;PORT=8099;"
    "CONTROLLER=localhost:9000",
    autocommit=True,
)
```

`autocommit=True` is recommended: Pinot has no transactions, and it skips
pyodbc's commit bookkeeping. (The driver tolerates either setting.)

## Queries

```python
cur = conn.cursor()

cur.execute("SELECT playerName, homeRuns FROM baseballStats ORDER BY homeRuns DESC LIMIT 5")
for row in cur.fetchall():
    print(row.playerName, row.homeRuns)

# Parameterized queries: '?' markers are substituted client-side as
# escaped literals (Pinot's HTTP API has no server-side prepare).
cur.execute(
    "SELECT count(*) FROM baseballStats WHERE yearID >= ? AND league = ?",
    2000, "NL",
)
print(cur.fetchone()[0])
```

Pinot types arrive as natural Python types:

| Pinot | Python |
|-------|--------|
| `INT`, `LONG` | `int` |
| `FLOAT`, `DOUBLE` | `float` |
| `BIG_DECIMAL` | `decimal.Decimal` |
| `BOOLEAN` | `bool` |
| `TIMESTAMP` | `datetime.datetime` |
| `STRING`, `JSON` | `str` |
| `BYTES` | `bytes` |
| `*_ARRAY` (multi-value) | `str` (JSON text, e.g. `"[1,2,3]"` — `json.loads` it) |

`NULL` values arrive as `None`.

## Catalog discovery

```python
tables = [t.table_name for t in cur.tables()]
columns = [(c.column_name, c.type_name) for c in cur.columns(table="baseballStats")]
pks = [r.column_name for r in cur.primaryKeys(table="myUpsertTable")]
```

## pandas

```python
import pandas as pd

df = pd.read_sql("SELECT * FROM baseballStats LIMIT 1000", conn)
df2 = pd.read_sql(
    "SELECT yearID, sum(homeRuns) AS hr FROM baseballStats GROUP BY yearID ORDER BY yearID",
    conn,
)
```

Notes:

- pandas prints a `UserWarning` that only SQLAlchemy connectables are
  officially supported — the pyodbc connection works fine; silence it with
  `warnings.filterwarnings("ignore", message="pandas only supports SQLAlchemy")`
  if it bothers you.
- `TIMESTAMP` columns land as `datetime64`; integer columns containing NULLs
  are upcast to `float64` by pandas (standard pandas behavior).
- Aggregate in Pinot (`GROUP BY` in SQL) rather than pulling raw rows — the
  driver buffers the broker's full response in memory.

## Tips

- **JOINs / window functions** need Pinot's multi-stage engine: add
  `USEMULTISTAGE=1` to the connection string.
- **Timeouts**: `TIMEOUT=120` (HTTP, seconds) and
  `QUERYOPTIONS=timeoutMs=120000` (Pinot-side) for long queries; or set
  `cur.timeout` per statement.
- **Auth**: `UID`/`PWD` for HTTP Basic, `TOKEN=...` for bearer tokens
  (see [StarTree Cloud](startree-cloud.md)).
- A runnable script lives at
  [examples/pyodbc_example.py](../examples/pyodbc_example.py).
