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

// Connection establishment, SQLGetInfo, SQLGetFunctions, SQLNativeSql.

#include <set>

#include <sql.h>
#include <sqlext.h>

#include "api_helpers.h"
#include "handles.h"

using namespace pinot_odbc;

namespace pinot_odbc {

// Shared by SQLConnect/SQLDriverConnect (and their W variants).
SQLRETURN connectWithKeyValues(PinotDbc* dbc, std::map<std::string, std::string> kv) {
  if (dbc->connected) {
    dbc->diag.add("08002", "Connection name in use (already connected)");
    return SQL_ERROR;
  }
  try {
    mergeDsnProfile(kv);
    dbc->cfg = configFromKeyValues(kv);
    if (dbc->loginTimeout > 0 && dbc->cfg.timeoutSec > static_cast<long>(dbc->loginTimeout)) {
      dbc->cfg.timeoutSec = static_cast<long>(dbc->loginTimeout);
    }
    dbc->client = std::make_unique<PinotClient>(dbc->cfg);
    if (dbc->cfg.healthCheck) {
      dbc->client->checkHealth();
    }
    dbc->connected = true;
    return SQL_SUCCESS;
  } catch (const DiagError& e) {
    dbc->client.reset();
    dbc->diag.add(e.sqlstate, e.message, e.native);
    return SQL_ERROR;
  } catch (const std::exception& e) {
    dbc->client.reset();
    dbc->diag.add("HY000", std::string("Unexpected error during connect: ") + e.what());
    return SQL_ERROR;
  }
}

}  // namespace pinot_odbc

extern "C" {

SQLRETURN SQL_API SQLConnect(SQLHDBC ConnectionHandle, SQLCHAR* ServerName,
                             SQLSMALLINT NameLength1, SQLCHAR* UserName, SQLSMALLINT NameLength2,
                             SQLCHAR* Authentication, SQLSMALLINT NameLength3) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  std::map<std::string, std::string> kv;
  std::string dsn = fromSqlString(ServerName, NameLength1);
  std::string uid = fromSqlString(UserName, NameLength2);
  std::string pwd = fromSqlString(Authentication, NameLength3);
  if (!dsn.empty()) kv["DSN"] = dsn;
  if (!uid.empty()) kv["UID"] = uid;
  if (!pwd.empty()) kv["PWD"] = pwd;
  return connectWithKeyValues(dbc, std::move(kv));
}

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC ConnectionHandle, SQLHWND WindowHandle,
                                   SQLCHAR* InConnectionString, SQLSMALLINT StringLength1,
                                   SQLCHAR* OutConnectionString, SQLSMALLINT BufferLength,
                                   SQLSMALLINT* StringLength2Ptr, SQLUSMALLINT DriverCompletion) {
  (void)WindowHandle;
  (void)DriverCompletion;  // no dialog support; treat everything as NOPROMPT
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();

  std::string connStr = fromSqlString(InConnectionString, StringLength1);
  SQLRETURN rc = connectWithKeyValues(dbc, parseConnectionString(connStr));
  if (!SQL_SUCCEEDED(rc)) return rc;

  std::string outStr = buildOutConnectionString(dbc->cfg);
  SQLRETURN copyRc = copyStringToBuffer(dbc->diag, outStr, OutConnectionString,
                                        BufferLength, StringLength2Ptr);
  return copyRc == SQL_SUCCESS ? rc : copyRc;
}

SQLRETURN SQL_API SQLBrowseConnect(SQLHDBC ConnectionHandle, SQLCHAR* InConnectionString,
                                   SQLSMALLINT StringLength1, SQLCHAR* OutConnectionString,
                                   SQLSMALLINT BufferLength, SQLSMALLINT* StringLength2Ptr) {
  (void)InConnectionString;
  (void)StringLength1;
  (void)OutConnectionString;
  (void)BufferLength;
  (void)StringLength2Ptr;
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  dbc->diag.add("HYC00", "SQLBrowseConnect is not supported");
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC ConnectionHandle) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  if (!dbc->connected) {
    dbc->diag.add("08003", "Connection not open");
    return SQL_ERROR;
  }
  // Statements are implicitly dropped on disconnect.
  for (PinotStmt* stmt : dbc->stmts) {
    stmt->magic = kDeadMagic;
    delete stmt;
  }
  dbc->stmts.clear();
  dbc->client.reset();
  dbc->connected = false;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNativeSql(SQLHDBC ConnectionHandle, SQLCHAR* InStatementText,
                               SQLINTEGER TextLength1, SQLCHAR* OutStatementText,
                               SQLINTEGER BufferLength, SQLINTEGER* TextLength2Ptr) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  std::string sql = fromSqlString(InStatementText, TextLength1);
  return copyStringToBuffer(dbc->diag, sql, OutStatementText, BufferLength, TextLength2Ptr);
}

// ---- SQLGetInfo ---------------------------------------------------------------

SQLRETURN SQL_API SQLGetInfo(SQLHDBC ConnectionHandle, SQLUSMALLINT InfoType,
                             SQLPOINTER InfoValue, SQLSMALLINT BufferLength,
                             SQLSMALLINT* StringLengthPtr) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();

  auto str = [&](const std::string& s) -> SQLRETURN {
    return copyStringToBuffer(dbc->diag, s, static_cast<SQLCHAR*>(InfoValue), BufferLength,
                              StringLengthPtr);
  };
  auto u16 = [&](SQLUSMALLINT v) -> SQLRETURN {
    if (InfoValue != nullptr) *static_cast<SQLUSMALLINT*>(InfoValue) = v;
    if (StringLengthPtr != nullptr) *StringLengthPtr = sizeof(SQLUSMALLINT);
    return SQL_SUCCESS;
  };
  auto u32 = [&](SQLUINTEGER v) -> SQLRETURN {
    if (InfoValue != nullptr) *static_cast<SQLUINTEGER*>(InfoValue) = v;
    if (StringLengthPtr != nullptr) *StringLengthPtr = sizeof(SQLUINTEGER);
    return SQL_SUCCESS;
  };
  auto ulen = [&](SQLULEN v) -> SQLRETURN {
    if (InfoValue != nullptr) *static_cast<SQLULEN*>(InfoValue) = v;
    if (StringLengthPtr != nullptr) *StringLengthPtr = sizeof(SQLULEN);
    return SQL_SUCCESS;
  };

  switch (InfoType) {
    // ---- identification ----
    case SQL_DRIVER_NAME: return str("libpinot_odbc");
    case SQL_DRIVER_VER: return str("00.01.0000");
    case SQL_DRIVER_ODBC_VER: return str("03.51");
    case SQL_DBMS_NAME: return str("Apache Pinot");
    case SQL_DBMS_VER: return str("01.00.0000");
    case SQL_DATA_SOURCE_NAME: return str(dbc->cfg.dsn);
    case SQL_SERVER_NAME: return str(dbc->cfg.host);
    case SQL_USER_NAME: return str(dbc->cfg.uid);
    case SQL_DATABASE_NAME: return str(dbc->cfg.database);

    // ---- catalog/schema support ----
    case SQL_CATALOG_NAME: return str("N");
    case SQL_CATALOG_TERM: return str("");
    case SQL_CATALOG_NAME_SEPARATOR: return str(".");
    case SQL_CATALOG_USAGE: return u32(0);
    case SQL_CATALOG_LOCATION: return u16(0);
    case SQL_SCHEMA_TERM: return str("");
    case SQL_SCHEMA_USAGE: return u32(0);
    case SQL_TABLE_TERM: return str("table");
    case SQL_PROCEDURE_TERM: return str("");
    case SQL_PROCEDURES: return str("N");
    case SQL_ACCESSIBLE_TABLES: return str("Y");
    case SQL_ACCESSIBLE_PROCEDURES: return str("N");

    // ---- identifiers / literals ----
    case SQL_IDENTIFIER_QUOTE_CHAR: return str("\"");
    case SQL_IDENTIFIER_CASE: return u16(SQL_IC_SENSITIVE);
    case SQL_QUOTED_IDENTIFIER_CASE: return u16(SQL_IC_SENSITIVE);
    case SQL_SPECIAL_CHARACTERS: return str("");
    case SQL_SEARCH_PATTERN_ESCAPE: return str("\\");
    case SQL_LIKE_ESCAPE_CLAUSE: return str("N");
    case SQL_KEYWORDS: return str("");
    case SQL_COLUMN_ALIAS: return str("Y");
    case SQL_ORDER_BY_COLUMNS_IN_SELECT: return str("N");
    case SQL_EXPRESSIONS_IN_ORDERBY: return str("Y");
    case SQL_CONCAT_NULL_BEHAVIOR: return u16(SQL_CB_NULL);
    case SQL_GROUP_BY: return u16(SQL_GB_GROUP_BY_CONTAINS_SELECT);
    case SQL_NON_NULLABLE_COLUMNS: return u16(SQL_NNC_NON_NULL);
    case SQL_NULL_COLLATION: return u16(SQL_NC_LOW);
    case SQL_CORRELATION_NAME: return u16(SQL_CN_ANY);

    // ---- transactions (none) ----
    case SQL_TXN_CAPABLE: return u16(SQL_TC_NONE);
    case SQL_DEFAULT_TXN_ISOLATION: return u32(0);
    case SQL_TXN_ISOLATION_OPTION: return u32(0);
    case SQL_MULTIPLE_ACTIVE_TXN: return str("N");
    case SQL_DATA_SOURCE_READ_ONLY: return str("Y");
    case SQL_CURSOR_COMMIT_BEHAVIOR: return u16(SQL_CB_PRESERVE);
    case SQL_CURSOR_ROLLBACK_BEHAVIOR: return u16(SQL_CB_PRESERVE);

    // ---- cursors / fetching ----
    case SQL_SCROLL_OPTIONS: return u32(SQL_SO_FORWARD_ONLY);
    case SQL_CURSOR_SENSITIVITY: return u32(SQL_INSENSITIVE);
    case SQL_GETDATA_EXTENSIONS:
      return u32(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND);
    case SQL_BOOKMARK_PERSISTENCE: return u32(0);
    case SQL_ROW_UPDATES: return str("N");
    case SQL_POS_OPERATIONS: return u32(0);
    case SQL_POSITIONED_STATEMENTS: return u32(0);
    case SQL_LOCK_TYPES: return u32(0);
    case SQL_STATIC_SENSITIVITY: return u32(0);
    case SQL_BATCH_SUPPORT: return u32(0);
    case SQL_BATCH_ROW_COUNT: return u32(0);
    case SQL_PARAM_ARRAY_ROW_COUNTS: return u32(SQL_PARC_NO_BATCH);
    case SQL_PARAM_ARRAY_SELECTS: return u32(SQL_PAS_NO_SELECT);
    case SQL_MULT_RESULT_SETS: return str("N");
    case SQL_MAX_CONCURRENT_ACTIVITIES: return u16(0);
    case SQL_MAX_DRIVER_CONNECTIONS: return u16(0);
    case SQL_ACTIVE_ENVIRONMENTS: return u16(0);
    case SQL_ASYNC_MODE: return u32(SQL_AM_NONE);
    case SQL_NEED_LONG_DATA_LEN: return str("N");
    case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS: return u32(0);

    // ---- limits (0 = unknown / no limit) ----
    case SQL_MAX_COLUMN_NAME_LEN: return u16(128);
    case SQL_MAX_TABLE_NAME_LEN: return u16(128);
    case SQL_MAX_SCHEMA_NAME_LEN: return u16(0);
    case SQL_MAX_CATALOG_NAME_LEN: return u16(0);
    case SQL_MAX_CURSOR_NAME_LEN: return u16(64);
    case SQL_MAX_IDENTIFIER_LEN: return u16(128);
    case SQL_MAX_COLUMNS_IN_GROUP_BY: return u16(0);
    case SQL_MAX_COLUMNS_IN_ORDER_BY: return u16(0);
    case SQL_MAX_COLUMNS_IN_SELECT: return u16(0);
    case SQL_MAX_COLUMNS_IN_TABLE: return u16(0);
    case SQL_MAX_COLUMNS_IN_INDEX: return u16(0);
    case SQL_MAX_TABLES_IN_SELECT: return u16(0);
    case SQL_MAX_USER_NAME_LEN: return u16(0);
    case SQL_MAX_ROW_SIZE: return u32(0);
    case SQL_MAX_ROW_SIZE_INCLUDES_LONG: return str("Y");
    case SQL_MAX_STATEMENT_LEN: return u32(0);
    case SQL_MAX_CHAR_LITERAL_LEN: return u32(0);
    case SQL_MAX_BINARY_LITERAL_LEN: return u32(0);
    case SQL_MAX_INDEX_SIZE: return u32(0);
    case SQL_FILE_USAGE: return u16(SQL_FILE_NOT_SUPPORTED);

    // ---- SQL support ----
    case SQL_SQL_CONFORMANCE: return u32(SQL_SC_SQL92_ENTRY);
    case SQL_ODBC_INTERFACE_CONFORMANCE: return u32(SQL_OIC_CORE);
    case SQL_ODBC_API_CONFORMANCE: return u16(SQL_OAC_LEVEL1);
    case SQL_ODBC_SQL_CONFORMANCE: return u16(SQL_OSC_MINIMUM);
    case SQL_FETCH_DIRECTION: return u32(SQL_FD_FETCH_NEXT);
    case SQL_OUTER_JOINS: return str("Y");
    case SQL_OJ_CAPABILITIES:
      return u32(SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NOT_ORDERED |
                 SQL_OJ_ALL_COMPARISON_OPS);
    case SQL_SQL92_RELATIONAL_JOIN_OPERATORS:
      return u32(SQL_SRJO_INNER_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_RIGHT_OUTER_JOIN |
                 SQL_SRJO_FULL_OUTER_JOIN | SQL_SRJO_CROSS_JOIN);
    case SQL_SQL92_PREDICATES:
      return u32(SQL_SP_BETWEEN | SQL_SP_COMPARISON | SQL_SP_IN | SQL_SP_ISNOTNULL |
                 SQL_SP_ISNULL | SQL_SP_LIKE);
    case SQL_SQL92_VALUE_EXPRESSIONS:
      return u32(SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF);
    case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS: return u32(0);
    case SQL_SQL92_STRING_FUNCTIONS: return u32(0);
    case SQL_SQL92_DATETIME_FUNCTIONS: return u32(0);
    case SQL_SQL92_GRANT: return u32(0);
    case SQL_SQL92_REVOKE: return u32(0);
    case SQL_SQL92_ROW_VALUE_CONSTRUCTOR: return u32(0);
    case SQL_SQL92_FOREIGN_KEY_DELETE_RULE: return u32(0);
    case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE: return u32(0);
    case SQL_AGGREGATE_FUNCTIONS:
      return u32(SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_DISTINCT | SQL_AF_MAX |
                 SQL_AF_MIN | SQL_AF_SUM);
    case SQL_NUMERIC_FUNCTIONS:
      return u32(SQL_FN_NUM_ABS | SQL_FN_NUM_CEILING | SQL_FN_NUM_EXP | SQL_FN_NUM_FLOOR |
                 SQL_FN_NUM_LOG | SQL_FN_NUM_LOG10 | SQL_FN_NUM_MOD | SQL_FN_NUM_POWER |
                 SQL_FN_NUM_SIGN | SQL_FN_NUM_SQRT);
    case SQL_STRING_FUNCTIONS:
      return u32(SQL_FN_STR_CONCAT | SQL_FN_STR_LENGTH | SQL_FN_STR_LCASE | SQL_FN_STR_LTRIM |
                 SQL_FN_STR_REPLACE | SQL_FN_STR_RTRIM | SQL_FN_STR_SUBSTRING |
                 SQL_FN_STR_UCASE);
    case SQL_TIMEDATE_FUNCTIONS:
      return u32(SQL_FN_TD_NOW | SQL_FN_TD_YEAR | SQL_FN_TD_MONTH | SQL_FN_TD_DAYOFMONTH |
                 SQL_FN_TD_HOUR | SQL_FN_TD_MINUTE | SQL_FN_TD_SECOND);
    case SQL_TIMEDATE_ADD_INTERVALS: return u32(0);
    case SQL_TIMEDATE_DIFF_INTERVALS: return u32(0);
    case SQL_SYSTEM_FUNCTIONS: return u32(0);
    case SQL_CONVERT_FUNCTIONS: return u32(SQL_FN_CVT_CAST);
    case SQL_UNION: return u32(SQL_U_UNION | SQL_U_UNION_ALL);
    case SQL_SUBQUERIES:
      return u32(SQL_SQ_COMPARISON | SQL_SQ_IN | SQL_SQ_EXISTS);
    case SQL_DESCRIBE_PARAMETER: return str("N");
    case SQL_INTEGRITY: return str("N");

    // ---- DDL (none) ----
    case SQL_CREATE_TABLE: return u32(0);
    case SQL_DROP_TABLE: return u32(0);
    case SQL_ALTER_TABLE: return u32(0);
    case SQL_CREATE_VIEW: return u32(0);
    case SQL_DROP_VIEW: return u32(0);
    case SQL_CREATE_SCHEMA: return u32(0);
    case SQL_DROP_SCHEMA: return u32(0);
    case SQL_INSERT_STATEMENT: return u32(0);

    // ---- type conversion matrix: report CAST-style convertibility ----
    case SQL_CONVERT_BIGINT:
    case SQL_CONVERT_BIT:
    case SQL_CONVERT_CHAR:
    case SQL_CONVERT_DECIMAL:
    case SQL_CONVERT_DOUBLE:
    case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_INTEGER:
    case SQL_CONVERT_LONGVARCHAR:
    case SQL_CONVERT_NUMERIC:
    case SQL_CONVERT_REAL:
    case SQL_CONVERT_SMALLINT:
    case SQL_CONVERT_TIMESTAMP:
    case SQL_CONVERT_TINYINT:
    case SQL_CONVERT_VARBINARY:
    case SQL_CONVERT_VARCHAR:
      return u32(SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR | SQL_CVT_INTEGER |
                 SQL_CVT_BIGINT | SQL_CVT_FLOAT | SQL_CVT_DOUBLE | SQL_CVT_TIMESTAMP);
    case SQL_CONVERT_BINARY:
    case SQL_CONVERT_DATE:
    case SQL_CONVERT_TIME:
    case SQL_CONVERT_LONGVARBINARY:
    case SQL_CONVERT_WCHAR:
    case SQL_CONVERT_WVARCHAR:
    case SQL_CONVERT_WLONGVARCHAR:
    case SQL_CONVERT_GUID:
    case SQL_CONVERT_INTERVAL_DAY_TIME:
    case SQL_CONVERT_INTERVAL_YEAR_MONTH:
      return u32(0);

    // ---- misc ----
    case SQL_DTC_TRANSITION_COST: return u32(0);
    case SQL_INFO_SCHEMA_VIEWS: return u32(0);
    case SQL_DDL_INDEX: return u32(0);
    case SQL_KEYSET_CURSOR_ATTRIBUTES1:
    case SQL_KEYSET_CURSOR_ATTRIBUTES2:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES2:
      return u32(0);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
      return u32(SQL_CA1_NEXT);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2:
      return u32(SQL_CA2_READ_ONLY_CONCURRENCY);
    case SQL_STATIC_CURSOR_ATTRIBUTES1:
    case SQL_STATIC_CURSOR_ATTRIBUTES2:
      return u32(0);
    case SQL_XOPEN_CLI_YEAR: return str("1995");
    case SQL_STANDARD_CLI_CONFORMANCE: return u32(0);
    case SQL_DM_VER: return str("");
    case SQL_COLLATION_SEQ: return str("");
    case SQL_DRIVER_HDBC:
    case SQL_DRIVER_HENV:
      // Filled by the driver manager, never the driver.
      return ulen(0);

    default:
      dbc->diag.add("HY096", "Information type out of range: " + std::to_string(InfoType));
      return SQL_ERROR;
  }
}

// ---- SQLGetFunctions ------------------------------------------------------------

static const SQLUSMALLINT kSupportedFunctions[] = {
    SQL_API_SQLALLOCHANDLE,   SQL_API_SQLFREEHANDLE,    SQL_API_SQLFREESTMT,
    SQL_API_SQLCLOSECURSOR,   SQL_API_SQLSETENVATTR,    SQL_API_SQLGETENVATTR,
    SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR, SQL_API_SQLSETSTMTATTR,
    SQL_API_SQLGETSTMTATTR,   SQL_API_SQLCONNECT,       SQL_API_SQLDRIVERCONNECT,
    SQL_API_SQLDISCONNECT,    SQL_API_SQLGETINFO,       SQL_API_SQLGETFUNCTIONS,
    SQL_API_SQLGETTYPEINFO,   SQL_API_SQLEXECDIRECT,    SQL_API_SQLPREPARE,
    SQL_API_SQLEXECUTE,       SQL_API_SQLNUMPARAMS,     SQL_API_SQLBINDPARAMETER,
    SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLDESCRIBECOL,
    SQL_API_SQLCOLATTRIBUTE,  SQL_API_SQLBINDCOL,       SQL_API_SQLFETCH,
    SQL_API_SQLFETCHSCROLL,   SQL_API_SQLGETDATA,       SQL_API_SQLROWCOUNT,
    SQL_API_SQLMORERESULTS,   SQL_API_SQLCANCEL,        SQL_API_SQLENDTRAN,
    SQL_API_SQLGETDIAGREC,    SQL_API_SQLGETDIAGFIELD,  SQL_API_SQLTABLES,
    SQL_API_SQLCOLUMNS,       SQL_API_SQLPRIMARYKEYS,   SQL_API_SQLFOREIGNKEYS,
    SQL_API_SQLSTATISTICS,    SQL_API_SQLSPECIALCOLUMNS, SQL_API_SQLPROCEDURES,
    SQL_API_SQLPROCEDURECOLUMNS, SQL_API_SQLTABLEPRIVILEGES, SQL_API_SQLCOLUMNPRIVILEGES,
    SQL_API_SQLNATIVESQL,     SQL_API_SQLGETCURSORNAME, SQL_API_SQLSETCURSORNAME,
};

SQLRETURN SQL_API SQLGetFunctions(SQLHDBC ConnectionHandle, SQLUSMALLINT FunctionId,
                                  SQLUSMALLINT* Supported) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  if (Supported == nullptr) return SQL_ERROR;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();

  static const std::set<SQLUSMALLINT> kSet(std::begin(kSupportedFunctions),
                                           std::end(kSupportedFunctions));
  if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
    std::memset(Supported, 0, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * sizeof(SQLUSMALLINT));
    for (SQLUSMALLINT id : kSet) {
      Supported[id >> 4] = static_cast<SQLUSMALLINT>(Supported[id >> 4] | (1 << (id & 0xF)));
    }
    return SQL_SUCCESS;
  }
  if (FunctionId == SQL_API_ALL_FUNCTIONS) {
    std::memset(Supported, 0, 100 * sizeof(SQLUSMALLINT));
    for (SQLUSMALLINT id : kSet) {
      if (id < 100) Supported[id] = SQL_TRUE;
    }
    return SQL_SUCCESS;
  }
  *Supported = kSet.count(FunctionId) ? SQL_TRUE : SQL_FALSE;
  return SQL_SUCCESS;
}

}  // extern "C"
