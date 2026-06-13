# Using Apache Pinot from R

The [`odbc`](https://odbc.r-dbi.org/) package implements R's standard
[DBI](https://dbi.r-dbi.org/) interface over the system driver manager.

## Setup

Install the driver and register it (see the
[README](../README.md#installing-the-driver)), then:

```r
install.packages(c("DBI", "odbc"))
```

macOS/Linux builds of the `odbc` package link against unixODBC
(`brew install unixodbc` / `apt install unixodbc-dev`).

## Connecting and querying

```r
library(DBI)

con <- dbConnect(odbc::odbc(),
                 driver     = "PinotODBC",          # name from odbcinst.ini
                 host       = "localhost",
                 port       = 8099,
                 controller = "localhost:9000")
# or via DSN: dbConnect(odbc::odbc(), dsn = "Pinot")
# or a raw string: dbConnect(odbc::odbc(),
#   .connection_string = "DRIVER=PinotODBC;HOST=localhost;PORT=8099;CONTROLLER=localhost:9000")

df <- dbGetQuery(con, "
  SELECT yearID, SUM(homeRuns) AS hr
  FROM baseballStats
  GROUP BY yearID
  ORDER BY yearID")
head(df)

# Parameterized queries ('?' markers, substituted client-side):
dbGetQuery(con,
           "SELECT count(*) FROM baseballStats WHERE yearID >= ? AND league = ?",
           params = list(2000L, "NL"))

# Catalog discovery:
dbListTables(con)
dbListFields(con, "baseballStats")

dbDisconnect(con)
```

`dbGetQuery` returns a `data.frame` with natural R types: Pinot
`INT`/`LONG` → `integer`/`numeric` (`integer64` for LONG via the `bit64`
package), `DOUBLE` → `numeric`, `BOOLEAN` → `logical`, `TIMESTAMP` →
`POSIXct`, `STRING`/`JSON`/`*_ARRAY` → `character`, and `NULL` → `NA`.

## Notes

- Aggregate in Pinot (SQL `GROUP BY`) rather than pulling raw rows — the
  driver buffers the broker's full response, and so does R.
- `dplyr`/`dbplyr` lazy tables (`tbl(con, "baseballStats")`) work for simple
  verbs, but generated SQL beyond filters/aggregations may exceed Pinot's
  dialect — prefer explicit `dbGetQuery` SQL for anything complex.
- JOINs need the multi-stage engine: add `usemultistage = 1` (or
  `USEMULTISTAGE=1` in the connection string).
- Auth: `uid`/`pwd` arguments (HTTP Basic) or `token = "<bearer>"` — see
  [StarTree Cloud](startree-cloud.md).
