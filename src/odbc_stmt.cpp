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

// Statement execution, result description, fetching, and data retrieval.

#include <sql.h>
#include <sqlext.h>

#include "api_helpers.h"
#include "convert.h"
#include "handles.h"

using namespace pinot_odbc;

namespace {

SQLRETURN runStatement(PinotStmt* stmt, const std::string& sql) {
  try {
    executeStatement(stmt, sql);
    return SQL_SUCCESS;
  } catch (const DiagError& e) {
    stmt->diag.add(e.sqlstate, e.message, e.native);
    return SQL_ERROR;
  } catch (const std::exception& e) {
    stmt->diag.add("HY000", std::string("Unexpected error: ") + e.what());
    return SQL_ERROR;
  }
}

SQLRETURN fetchNextRow(PinotStmt* stmt) {
  if (!stmt->hasResult) {
    stmt->diag.add("24000", "Invalid cursor state (no open result set)");
    return SQL_ERROR;
  }
  if (stmt->cursor + 1 >= static_cast<long>(stmt->rs.rowCount())) {
    stmt->cursor = static_cast<long>(stmt->rs.rowCount());
    if (stmt->rowsFetchedPtr != nullptr) *stmt->rowsFetchedPtr = 0;
    return SQL_NO_DATA;
  }
  stmt->cursor++;
  stmt->getDataOffsets.clear();

  SQLRETURN result = SQL_SUCCESS;
  for (const auto& [col, binding] : stmt->bindings) {
    if (col == 0 || col > stmt->rs.columns.size()) {
      stmt->diag.add("07009", "Invalid descriptor index for bound column " + std::to_string(col));
      result = SQL_ERROR;
      continue;
    }
    SQLRETURN rc = convertCell(stmt, col, binding.cType, binding.ptr, binding.bufLen,
                               binding.indPtr, /*isGetData=*/false);
    if (rc == SQL_ERROR) result = SQL_ERROR;
    else if (rc == SQL_SUCCESS_WITH_INFO && result == SQL_SUCCESS) result = SQL_SUCCESS_WITH_INFO;
  }
  if (stmt->rowsFetchedPtr != nullptr) *stmt->rowsFetchedPtr = 1;
  if (stmt->rowStatusPtr != nullptr) {
    stmt->rowStatusPtr[0] = (result == SQL_ERROR)
                                ? SQL_ROW_ERROR
                                : (result == SQL_SUCCESS_WITH_INFO ? SQL_ROW_SUCCESS_WITH_INFO
                                                                   : SQL_ROW_SUCCESS);
  }
  return result;
}

}  // namespace

extern "C" {

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT StatementHandle, SQLCHAR* StatementText,
                                SQLINTEGER TextLength) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  std::string sql = fromSqlString(StatementText, TextLength);
  if (trimAscii(sql).empty()) {
    stmt->diag.add("HY009", "Empty statement text");
    return SQL_ERROR;
  }
  stmt->sql = sql;
  stmt->prepared = false;
  return runStatement(stmt, sql);
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT StatementHandle, SQLCHAR* StatementText,
                             SQLINTEGER TextLength) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  std::string sql = fromSqlString(StatementText, TextLength);
  if (trimAscii(sql).empty()) {
    stmt->diag.add("HY009", "Empty statement text");
    return SQL_ERROR;
  }
  // Pinot has no server-side prepare; execution is deferred to SQLExecute.
  stmt->sql = sql;
  stmt->prepared = true;
  stmt->rs.clear();
  stmt->hasResult = false;
  stmt->cursor = -1;
  stmt->getDataOffsets.clear();
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLExecute(SQLHSTMT StatementHandle) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (!stmt->prepared || stmt->sql.empty()) {
    stmt->diag.add("HY010", "Function sequence error: statement not prepared");
    return SQL_ERROR;
  }
  return runStatement(stmt, stmt->sql);
}

SQLRETURN SQL_API SQLNumParams(SQLHSTMT StatementHandle, SQLSMALLINT* ParameterCountPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (ParameterCountPtr != nullptr) {
    *ParameterCountPtr = static_cast<SQLSMALLINT>(countParameterMarkers(stmt->sql));
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLBindParameter(SQLHSTMT StatementHandle, SQLUSMALLINT ParameterNumber,
                                   SQLSMALLINT InputOutputType, SQLSMALLINT ValueType,
                                   SQLSMALLINT ParameterType, SQLULEN ColumnSize,
                                   SQLSMALLINT DecimalDigits, SQLPOINTER ParameterValuePtr,
                                   SQLLEN BufferLength, SQLLEN* StrLen_or_IndPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (ParameterNumber < 1) {
    stmt->diag.add("07009", "Invalid descriptor index");
    return SQL_ERROR;
  }
  if (InputOutputType != SQL_PARAM_INPUT) {
    stmt->diag.add("HYC00", "Only input parameters are supported");
    return SQL_ERROR;
  }
  ParamBinding p;
  p.ioType = InputOutputType;
  p.cType = ValueType;
  p.sqlType = ParameterType;
  p.columnSize = ColumnSize;
  p.decimalDigits = DecimalDigits;
  p.ptr = ParameterValuePtr;
  p.bufLen = BufferLength;
  p.indPtr = StrLen_or_IndPtr;
  stmt->params[ParameterNumber] = p;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT StatementHandle, SQLUSMALLINT ParameterNumber,
                                   SQLSMALLINT* DataTypePtr, SQLULEN* ParameterSizePtr,
                                   SQLSMALLINT* DecimalDigitsPtr, SQLSMALLINT* NullablePtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  (void)ParameterNumber;
  // Parameter types cannot be inferred without server-side prepare.
  if (DataTypePtr != nullptr) *DataTypePtr = SQL_VARCHAR;
  if (ParameterSizePtr != nullptr) {
    *ParameterSizePtr = static_cast<SQLULEN>(stmt->dbc->cfg.stringColumnSize);
  }
  if (DecimalDigitsPtr != nullptr) *DecimalDigitsPtr = 0;
  if (NullablePtr != nullptr) *NullablePtr = SQL_NULLABLE;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT StatementHandle, SQLSMALLINT* ColumnCountPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (ColumnCountPtr != nullptr) {
    *ColumnCountPtr = static_cast<SQLSMALLINT>(stmt->rs.columns.size());
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                 SQLCHAR* ColumnName, SQLSMALLINT BufferLength,
                                 SQLSMALLINT* NameLengthPtr, SQLSMALLINT* DataTypePtr,
                                 SQLULEN* ColumnSizePtr, SQLSMALLINT* DecimalDigitsPtr,
                                 SQLSMALLINT* NullablePtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (ColumnNumber < 1 || ColumnNumber > stmt->rs.columns.size()) {
    stmt->diag.add("07009", "Invalid descriptor index " + std::to_string(ColumnNumber));
    return SQL_ERROR;
  }
  const Column& col = stmt->rs.columns[ColumnNumber - 1];
  if (DataTypePtr != nullptr) *DataTypePtr = col.meta.sqlType;
  if (ColumnSizePtr != nullptr) *ColumnSizePtr = col.meta.columnSize;
  if (DecimalDigitsPtr != nullptr) *DecimalDigitsPtr = col.meta.decimalDigits;
  if (NullablePtr != nullptr) *NullablePtr = SQL_NULLABLE_UNKNOWN;
  return copyStringToBuffer(stmt->diag, col.name, ColumnName, BufferLength, NameLengthPtr);
}

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                  SQLUSMALLINT FieldIdentifier, SQLPOINTER CharacterAttribute,
                                  SQLSMALLINT BufferLength, SQLSMALLINT* StringLength,
                                  SQLLEN* NumericAttribute) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();

  auto num = [&](SQLLEN v) -> SQLRETURN {
    if (NumericAttribute != nullptr) *NumericAttribute = v;
    return SQL_SUCCESS;
  };
  auto str = [&](const std::string& s) -> SQLRETURN {
    return copyStringToBuffer(stmt->diag, s, static_cast<SQLCHAR*>(CharacterAttribute),
                              BufferLength, StringLength);
  };

  if (FieldIdentifier == SQL_DESC_COUNT || FieldIdentifier == SQL_COLUMN_COUNT) {
    return num(static_cast<SQLLEN>(stmt->rs.columns.size()));
  }
  if (ColumnNumber < 1 || ColumnNumber > stmt->rs.columns.size()) {
    stmt->diag.add("07009", "Invalid descriptor index " + std::to_string(ColumnNumber));
    return SQL_ERROR;
  }
  const Column& col = stmt->rs.columns[ColumnNumber - 1];

  switch (FieldIdentifier) {
    case SQL_DESC_NAME:
    case SQL_DESC_LABEL:
    case SQL_DESC_BASE_COLUMN_NAME:
      return str(col.name);
    case SQL_DESC_TYPE:
      return num(col.meta.sqlDataType);
    case SQL_DESC_CONCISE_TYPE:
      return num(col.meta.sqlType);
    case SQL_DESC_DATETIME_INTERVAL_CODE:
      return num(col.meta.datetimeSub);
    case SQL_DESC_DISPLAY_SIZE:
      return num(col.meta.displaySize);
    case SQL_DESC_LENGTH:
    case SQL_COLUMN_LENGTH:  // ODBC 2.x id
      return num(static_cast<SQLLEN>(col.meta.columnSize));
    case SQL_DESC_OCTET_LENGTH:
      return num(col.meta.octetLength);
    case SQL_DESC_PRECISION:
    case SQL_COLUMN_PRECISION:
      return num(static_cast<SQLLEN>(col.meta.columnSize));
    case SQL_DESC_SCALE:
    case SQL_COLUMN_SCALE:
      return num(col.meta.decimalDigits);
    case SQL_DESC_NULLABLE:
      return num(SQL_NULLABLE_UNKNOWN);
    case SQL_DESC_UNSIGNED:
      return num(col.meta.isSigned ? SQL_FALSE : SQL_TRUE);
    case SQL_DESC_SEARCHABLE:
      return num(SQL_PRED_SEARCHABLE);
    case SQL_DESC_UPDATABLE:
      return num(SQL_ATTR_READONLY);
    case SQL_DESC_AUTO_UNIQUE_VALUE:
      return num(SQL_FALSE);
    case SQL_DESC_CASE_SENSITIVE:
      return num(col.meta.caseSensitive ? SQL_TRUE : SQL_FALSE);
    case SQL_DESC_FIXED_PREC_SCALE:
      return num(SQL_FALSE);
    case SQL_DESC_TYPE_NAME:
    case SQL_DESC_LOCAL_TYPE_NAME:
      return str(col.meta.typeName);
    case SQL_DESC_LITERAL_PREFIX:
      return str(col.meta.literalPrefix);
    case SQL_DESC_LITERAL_SUFFIX:
      return str(col.meta.literalSuffix);
    case SQL_DESC_NUM_PREC_RADIX:
      return num(col.meta.numPrecRadix);
    case SQL_DESC_UNNAMED:
      return num(col.name.empty() ? SQL_UNNAMED : SQL_NAMED);
    case SQL_DESC_CATALOG_NAME:
    case SQL_DESC_SCHEMA_NAME:
    case SQL_DESC_TABLE_NAME:
    case SQL_DESC_BASE_TABLE_NAME:
      return str("");
    default:
      stmt->diag.add("HY091", "Invalid descriptor field identifier " +
                                  std::to_string(FieldIdentifier));
      return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLBindCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                             SQLSMALLINT TargetType, SQLPOINTER TargetValue, SQLLEN BufferLength,
                             SQLLEN* StrLen_or_Ind) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (ColumnNumber < 1) {
    stmt->diag.add("07009", "Invalid descriptor index");
    return SQL_ERROR;
  }
  if (TargetValue == nullptr && StrLen_or_Ind == nullptr) {
    stmt->bindings.erase(ColumnNumber);
    return SQL_SUCCESS;
  }
  ColumnBinding b;
  b.cType = TargetType;
  b.ptr = TargetValue;
  b.bufLen = BufferLength;
  b.indPtr = StrLen_or_Ind;
  stmt->bindings[ColumnNumber] = b;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFetch(SQLHSTMT StatementHandle) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  return fetchNextRow(stmt);
}

SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT StatementHandle, SQLSMALLINT FetchOrientation,
                                 SQLLEN FetchOffset) {
  (void)FetchOffset;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (FetchOrientation != SQL_FETCH_NEXT) {
    stmt->diag.add("HY106", "Fetch type out of range (forward-only cursor)");
    return SQL_ERROR;
  }
  return fetchNextRow(stmt);
}

SQLRETURN SQL_API SQLGetData(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                             SQLSMALLINT TargetType, SQLPOINTER TargetValue, SQLLEN BufferLength,
                             SQLLEN* StrLen_or_Ind) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (!stmt->hasResult || stmt->cursor < 0 ||
      stmt->cursor >= static_cast<long>(stmt->rs.rowCount())) {
    stmt->diag.add("24000", "Invalid cursor state");
    return SQL_ERROR;
  }
  if (ColumnNumber < 1 || ColumnNumber > stmt->rs.columns.size()) {
    stmt->diag.add("07009", "Invalid descriptor index " + std::to_string(ColumnNumber));
    return SQL_ERROR;
  }
  return convertCell(stmt, ColumnNumber, TargetType, TargetValue, BufferLength, StrLen_or_Ind,
                     /*isGetData=*/true);
}

SQLRETURN SQL_API SQLRowCount(SQLHSTMT StatementHandle, SQLLEN* RowCountPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (RowCountPtr != nullptr) {
    *RowCountPtr = stmt->hasResult ? static_cast<SQLLEN>(stmt->rs.rowCount()) : 0;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLMoreResults(SQLHSTMT StatementHandle) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  return SQL_NO_DATA;
}

SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT StatementHandle, SQLCHAR* CursorName,
                                   SQLSMALLINT BufferLength, SQLSMALLINT* NameLengthPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  return copyStringToBuffer(stmt->diag, stmt->cursorName, CursorName, BufferLength,
                            NameLengthPtr);
}

SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT StatementHandle, SQLCHAR* CursorName,
                                   SQLSMALLINT NameLength) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  stmt->cursorName = fromSqlString(CursorName, NameLength);
  return SQL_SUCCESS;
}

// ---- unsupported data-at-execution / positioned update APIs ----

SQLRETURN SQL_API SQLParamData(SQLHSTMT StatementHandle, SQLPOINTER* Value) {
  (void)Value;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  stmt->diag.clear();
  stmt->diag.add("HY010", "Function sequence error: no data-at-execution parameters");
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLPutData(SQLHSTMT StatementHandle, SQLPOINTER Data, SQLLEN StrLen_or_Ind) {
  (void)Data;
  (void)StrLen_or_Ind;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  stmt->diag.clear();
  stmt->diag.add("HY010", "Function sequence error: no data-at-execution parameters");
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetPos(SQLHSTMT StatementHandle, SQLSETPOSIROW RowNumber,
                            SQLUSMALLINT Operation, SQLUSMALLINT LockType) {
  (void)RowNumber;
  (void)Operation;
  (void)LockType;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  stmt->diag.clear();
  stmt->diag.add("HYC00", "SQLSetPos is not supported");
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLBulkOperations(SQLHSTMT StatementHandle, SQLSMALLINT Operation) {
  (void)Operation;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  stmt->diag.clear();
  stmt->diag.add("HYC00", "SQLBulkOperations is not supported");
  return SQL_ERROR;
}

}  // extern "C"
