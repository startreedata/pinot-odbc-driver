/*
 * Copyright 2026 StarTree Inc.
 *
 * Licensed under the StarTree Community License (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.startree.ai/startree-community-license
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

#pragma once

#include <sql.h>
#include <sqlext.h>

#include "handles.h"

namespace pinot_odbc {

// Converts the cell at (stmt->cursor, col) into the application buffer.
//
// When isGetData is true, chunked retrieval state is kept in
// stmt->getDataOffsets so successive SQLGetData calls continue where the
// previous one stopped (returns SQL_NO_DATA once a column is consumed).
// Bound-column conversion (SQLFetch) passes isGetData = false.
SQLRETURN convertCell(PinotStmt* stmt, SQLUSMALLINT col, SQLSMALLINT cType, SQLPOINTER target,
                      SQLLEN bufLen, SQLLEN* indPtr, bool isGetData);

}  // namespace pinot_odbc
