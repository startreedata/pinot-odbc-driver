#!/usr/bin/env python3
# Copyright 2026 StarTree Inc.
#
# Licensed under the StarTree Community License (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy
# of the License at http://www.startree.ai/startree-community-license
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

"""Query Apache Pinot through the Pinot ODBC driver with pyodbc.

Works against a local Pinot quickstart (broker on 8099, controller on 9000):
    pip install pyodbc
    python examples/pyodbc_example.py
"""

import pyodbc

# Either use a DSN defined in odbc.ini ...
#   conn = pyodbc.connect("DSN=Pinot", autocommit=True)
# ... or a full connection string:
CONNECTION_STRING = (
    "DRIVER=PinotODBC;"
    "HOST=localhost;PORT=8099;SCHEME=http;"
    "CONTROLLER=localhost:9000"
)

conn = pyodbc.connect(CONNECTION_STRING, autocommit=True)
cur = conn.cursor()

print("== Tables ==")
tables = [t.table_name for t in cur.tables()]
print(tables)

if tables:
    table = tables[0]
    print(f"\n== Columns of {table} ==")
    for col in cur.columns(table=table):
        print(f"  {col.column_name:30s} {col.type_name}")

    print(f"\n== First 5 rows of {table} ==")
    cur.execute(f'SELECT * FROM "{table}" LIMIT 5')
    print([d[0] for d in cur.description])
    for row in cur.fetchall():
        print(tuple(row))

    # Parametrized query: '?' markers are substituted client-side.
    first_col = cur.columns(table=table).fetchone().column_name
    cur.execute(f'SELECT COUNT(*) FROM "{table}" WHERE "{first_col}" IS NOT NULL OR ? = ?', 1, 1)
    print(f"\nCOUNT(*): {cur.fetchone()[0]}")

conn.close()
