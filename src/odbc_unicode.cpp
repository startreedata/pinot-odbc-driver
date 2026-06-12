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

// Unicode (W) entry points. These wrap the ANSI implementations, converting
// between the driver manager's SQLWCHAR encoding and UTF-8.
//
// Length conventions follow the ODBC spec: name/connection-string buffers are
// measured in characters; SQLGetInfoW and character SQLColAttributeW fields
// are measured in bytes.

#include <set>
#include <vector>

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include "api_helpers.h"
#include "handles.h"
#include "unicode_conv.h"

using namespace pinot_odbc;

namespace {

// Holds a UTF-8 copy of a wide input string, exposing SQLCHAR*/length.
struct Utf8Arg {
  std::string s;
  Utf8Arg(const SQLWCHAR* w, SQLINTEGER lenChars) : s(wideToUtf8(w, lenChars)) {}
  SQLCHAR* ptr() { return s.empty() ? nullptr : reinterpret_cast<SQLCHAR*>(s.data()); }
  SQLSMALLINT len() const { return static_cast<SQLSMALLINT>(s.size()); }
  SQLINTEGER ilen() const { return static_cast<SQLINTEGER>(s.size()); }
};

DiagList* stmtDiag(SQLHSTMT h) {
  PinotStmt* stmt = asStmt(h);
  return stmt != nullptr ? &stmt->diag : nullptr;
}

DiagList* dbcDiag(SQLHDBC h) {
  PinotDbc* dbc = asDbc(h);
  return dbc != nullptr ? &dbc->diag : nullptr;
}

}  // namespace

extern "C" {

SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC ConnectionHandle, SQLHWND WindowHandle,
                                    SQLWCHAR* InConnectionString, SQLSMALLINT StringLength1,
                                    SQLWCHAR* OutConnectionString, SQLSMALLINT BufferLength,
                                    SQLSMALLINT* StringLength2Ptr, SQLUSMALLINT DriverCompletion) {
  Utf8Arg in(InConnectionString, StringLength1);
  std::vector<char> out(4096, '\0');
  SQLSMALLINT outLen = 0;
  SQLRETURN rc = SQLDriverConnect(ConnectionHandle, WindowHandle, in.ptr(), in.len(),
                                  reinterpret_cast<SQLCHAR*>(out.data()),
                                  static_cast<SQLSMALLINT>(out.size()), &outLen, DriverCompletion);
  if (!SQL_SUCCEEDED(rc)) return rc;
  DiagList* diag = dbcDiag(ConnectionHandle);
  if (diag == nullptr) return rc;
  SQLRETURN copyRc = copyWideToBufferChars(*diag, std::string(out.data()), OutConnectionString,
                                           BufferLength, StringLength2Ptr);
  return copyRc == SQL_SUCCESS ? rc : copyRc;
}

SQLRETURN SQL_API SQLConnectW(SQLHDBC ConnectionHandle, SQLWCHAR* ServerName,
                              SQLSMALLINT NameLength1, SQLWCHAR* UserName,
                              SQLSMALLINT NameLength2, SQLWCHAR* Authentication,
                              SQLSMALLINT NameLength3) {
  Utf8Arg dsn(ServerName, NameLength1);
  Utf8Arg uid(UserName, NameLength2);
  Utf8Arg pwd(Authentication, NameLength3);
  return SQLConnect(ConnectionHandle, dsn.ptr(), dsn.len(), uid.ptr(), uid.len(), pwd.ptr(),
                    pwd.len());
}

SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT StatementHandle, SQLWCHAR* StatementText,
                                 SQLINTEGER TextLength) {
  Utf8Arg sql(StatementText, TextLength);
  return SQLExecDirect(StatementHandle, sql.ptr(), sql.ilen());
}

SQLRETURN SQL_API SQLPrepareW(SQLHSTMT StatementHandle, SQLWCHAR* StatementText,
                              SQLINTEGER TextLength) {
  Utf8Arg sql(StatementText, TextLength);
  return SQLPrepare(StatementHandle, sql.ptr(), sql.ilen());
}

SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC ConnectionHandle, SQLWCHAR* InStatementText,
                                SQLINTEGER TextLength1, SQLWCHAR* OutStatementText,
                                SQLINTEGER BufferLength, SQLINTEGER* TextLength2Ptr) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  std::string sql = wideToUtf8(InStatementText, TextLength1);
  return copyWideToBufferChars(dbc->diag, sql, OutStatementText, BufferLength, TextLength2Ptr);
}

SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                  SQLWCHAR* ColumnName, SQLSMALLINT BufferLength,
                                  SQLSMALLINT* NameLengthPtr, SQLSMALLINT* DataTypePtr,
                                  SQLULEN* ColumnSizePtr, SQLSMALLINT* DecimalDigitsPtr,
                                  SQLSMALLINT* NullablePtr) {
  std::vector<char> name(1024, '\0');
  SQLSMALLINT nameLen = 0;
  SQLRETURN rc = SQLDescribeCol(StatementHandle, ColumnNumber,
                                reinterpret_cast<SQLCHAR*>(name.data()),
                                static_cast<SQLSMALLINT>(name.size()), &nameLen, DataTypePtr,
                                ColumnSizePtr, DecimalDigitsPtr, NullablePtr);
  if (!SQL_SUCCEEDED(rc)) return rc;
  DiagList* diag = stmtDiag(StatementHandle);
  if (diag == nullptr) return rc;
  SQLRETURN copyRc = copyWideToBufferChars(*diag, std::string(name.data()), ColumnName,
                                           BufferLength, NameLengthPtr);
  return copyRc == SQL_SUCCESS ? rc : copyRc;
}

SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                   SQLUSMALLINT FieldIdentifier, SQLPOINTER CharacterAttribute,
                                   SQLSMALLINT BufferLength, SQLSMALLINT* StringLength,
                                   SQLLEN* NumericAttribute) {
  static const std::set<SQLUSMALLINT> kStringFields = {
      SQL_DESC_NAME,          SQL_DESC_LABEL,         SQL_DESC_BASE_COLUMN_NAME,
      SQL_DESC_TYPE_NAME,     SQL_DESC_LOCAL_TYPE_NAME, SQL_DESC_LITERAL_PREFIX,
      SQL_DESC_LITERAL_SUFFIX, SQL_DESC_CATALOG_NAME, SQL_DESC_SCHEMA_NAME,
      SQL_DESC_TABLE_NAME,    SQL_DESC_BASE_TABLE_NAME};
  if (kStringFields.count(FieldIdentifier) == 0) {
    return SQLColAttribute(StatementHandle, ColumnNumber, FieldIdentifier, CharacterAttribute,
                           BufferLength, StringLength, NumericAttribute);
  }
  std::vector<char> value(1024, '\0');
  SQLSMALLINT valueLen = 0;
  SQLRETURN rc = SQLColAttribute(StatementHandle, ColumnNumber, FieldIdentifier, value.data(),
                                 static_cast<SQLSMALLINT>(value.size()), &valueLen,
                                 NumericAttribute);
  if (!SQL_SUCCEEDED(rc)) return rc;
  DiagList* diag = stmtDiag(StatementHandle);
  if (diag == nullptr) return rc;
  SQLRETURN copyRc =
      copyWideToBufferBytes(*diag, std::string(value.data()),
                            static_cast<SQLWCHAR*>(CharacterAttribute), BufferLength, StringLength);
  return copyRc == SQL_SUCCESS ? rc : copyRc;
}

SQLRETURN SQL_API SQLGetInfoW(SQLHDBC ConnectionHandle, SQLUSMALLINT InfoType,
                              SQLPOINTER InfoValue, SQLSMALLINT BufferLength,
                              SQLSMALLINT* StringLengthPtr) {
  static const std::set<SQLUSMALLINT> kStringInfo = {
      SQL_DRIVER_NAME, SQL_DRIVER_VER, SQL_DRIVER_ODBC_VER, SQL_DBMS_NAME, SQL_DBMS_VER,
      SQL_DATA_SOURCE_NAME, SQL_SERVER_NAME, SQL_USER_NAME, SQL_DATABASE_NAME,
      SQL_CATALOG_NAME, SQL_CATALOG_TERM, SQL_CATALOG_NAME_SEPARATOR, SQL_SCHEMA_TERM,
      SQL_TABLE_TERM, SQL_PROCEDURE_TERM, SQL_PROCEDURES, SQL_ACCESSIBLE_TABLES,
      SQL_ACCESSIBLE_PROCEDURES, SQL_IDENTIFIER_QUOTE_CHAR, SQL_SPECIAL_CHARACTERS,
      SQL_SEARCH_PATTERN_ESCAPE, SQL_LIKE_ESCAPE_CLAUSE, SQL_KEYWORDS, SQL_COLUMN_ALIAS,
      SQL_ORDER_BY_COLUMNS_IN_SELECT, SQL_EXPRESSIONS_IN_ORDERBY, SQL_MULTIPLE_ACTIVE_TXN,
      SQL_DATA_SOURCE_READ_ONLY, SQL_ROW_UPDATES, SQL_NEED_LONG_DATA_LEN, SQL_MULT_RESULT_SETS,
      SQL_OUTER_JOINS, SQL_DESCRIBE_PARAMETER, SQL_INTEGRITY, SQL_MAX_ROW_SIZE_INCLUDES_LONG,
      SQL_XOPEN_CLI_YEAR, SQL_DM_VER, SQL_COLLATION_SEQ};
  if (kStringInfo.count(InfoType) == 0) {
    return SQLGetInfo(ConnectionHandle, InfoType, InfoValue, BufferLength, StringLengthPtr);
  }
  std::vector<char> value(2048, '\0');
  SQLSMALLINT valueLen = 0;
  SQLRETURN rc = SQLGetInfo(ConnectionHandle, InfoType, value.data(),
                            static_cast<SQLSMALLINT>(value.size()), &valueLen);
  if (!SQL_SUCCEEDED(rc)) return rc;
  DiagList* diag = dbcDiag(ConnectionHandle);
  if (diag == nullptr) return rc;
  SQLRETURN copyRc = copyWideToBufferBytes(*diag, std::string(value.data()),
                                           static_cast<SQLWCHAR*>(InfoValue), BufferLength,
                                           StringLengthPtr);
  return copyRc == SQL_SUCCESS ? rc : copyRc;
}

SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                 SQLWCHAR* SqlState, SQLINTEGER* NativeError,
                                 SQLWCHAR* MessageText, SQLSMALLINT BufferLength,
                                 SQLSMALLINT* TextLength) {
  SQLCHAR state[6] = {0};
  std::vector<char> msg(4096, '\0');
  SQLSMALLINT msgLen = 0;
  SQLRETURN rc = SQLGetDiagRec(HandleType, Handle, RecNumber, state, NativeError,
                               reinterpret_cast<SQLCHAR*>(msg.data()),
                               static_cast<SQLSMALLINT>(msg.size()), &msgLen);
  if (rc == SQL_NO_DATA || rc == SQL_INVALID_HANDLE || rc == SQL_ERROR) return rc;
  if (SqlState != nullptr) {
    WString wstate = utf8ToWide(reinterpret_cast<char*>(state));
    wstate.resize(5, static_cast<SQLWCHAR>('0'));
    for (size_t i = 0; i < 5; i++) SqlState[i] = wstate[i];
    SqlState[5] = 0;
  }
  WString wmsg = utf8ToWide(std::string(msg.data()));
  if (TextLength != nullptr) *TextLength = static_cast<SQLSMALLINT>(wmsg.size());
  if (MessageText != nullptr && BufferLength > 0) {
    size_t ncopy = std::min(wmsg.size(), static_cast<size_t>(BufferLength - 1));
    std::memcpy(MessageText, wmsg.data(), ncopy * sizeof(SQLWCHAR));
    MessageText[ncopy] = 0;
    if (ncopy < wmsg.size()) return SQL_SUCCESS_WITH_INFO;
  }
  return rc;
}

SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                   SQLSMALLINT DiagIdentifier, SQLPOINTER DiagInfo,
                                   SQLSMALLINT BufferLength, SQLSMALLINT* StringLength) {
  switch (DiagIdentifier) {
    case SQL_DIAG_SQLSTATE:
    case SQL_DIAG_MESSAGE_TEXT:
    case SQL_DIAG_CLASS_ORIGIN:
    case SQL_DIAG_SUBCLASS_ORIGIN:
    case SQL_DIAG_CONNECTION_NAME:
    case SQL_DIAG_SERVER_NAME: {
      std::vector<char> value(4096, '\0');
      SQLSMALLINT valueLen = 0;
      SQLRETURN rc = SQLGetDiagField(HandleType, Handle, RecNumber, DiagIdentifier, value.data(),
                                     static_cast<SQLSMALLINT>(value.size()), &valueLen);
      if (!SQL_SUCCEEDED(rc)) return rc;
      WString w = utf8ToWide(std::string(value.data()));
      if (StringLength != nullptr) {
        *StringLength = static_cast<SQLSMALLINT>(w.size() * sizeof(SQLWCHAR));
      }
      if (DiagInfo != nullptr && BufferLength >= static_cast<SQLSMALLINT>(sizeof(SQLWCHAR))) {
        size_t capChars = static_cast<size_t>(BufferLength) / sizeof(SQLWCHAR);
        size_t ncopy = std::min(w.size(), capChars - 1);
        std::memcpy(DiagInfo, w.data(), ncopy * sizeof(SQLWCHAR));
        static_cast<SQLWCHAR*>(DiagInfo)[ncopy] = 0;
        if (ncopy < w.size()) return SQL_SUCCESS_WITH_INFO;
      }
      return rc;
    }
    default:
      return SQLGetDiagField(HandleType, Handle, RecNumber, DiagIdentifier, DiagInfo,
                             BufferLength, StringLength);
  }
}

SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                     SQLPOINTER Value, SQLINTEGER StringLength) {
  // None of the supported attributes carry string values that we act on.
  return SQLSetConnectAttr(ConnectionHandle, Attribute, Value, StringLength);
}

SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                     SQLPOINTER Value, SQLINTEGER BufferLength,
                                     SQLINTEGER* StringLength) {
  if (Attribute == SQL_ATTR_CURRENT_CATALOG) {
    PinotDbc* dbc = asDbc(ConnectionHandle);
    if (dbc == nullptr) return SQL_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
    dbc->diag.clear();
    return copyWideToBufferBytes(dbc->diag, dbc->cfg.database, static_cast<SQLWCHAR*>(Value),
                                 BufferLength, StringLength);
  }
  return SQLGetConnectAttr(ConnectionHandle, Attribute, Value, BufferLength, StringLength);
}

SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                  SQLINTEGER StringLength) {
  return SQLSetStmtAttr(StatementHandle, Attribute, Value, StringLength);
}

SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                  SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  return SQLGetStmtAttr(StatementHandle, Attribute, Value, BufferLength, StringLength);
}

SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT StatementHandle, SQLSMALLINT DataType) {
  return SQLGetTypeInfo(StatementHandle, DataType);
}

SQLRETURN SQL_API SQLTablesW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                             SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                             SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                             SQLSMALLINT NameLength3, SQLWCHAR* TableType,
                             SQLSMALLINT NameLength4) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  Utf8Arg type(TableType, NameLength4);
  return SQLTables(StatementHandle, CatalogName != nullptr ? cat.ptr() : nullptr, cat.len(),
                   SchemaName != nullptr ? schema.ptr() : nullptr, schema.len(),
                   TableName != nullptr ? table.ptr() : nullptr, table.len(),
                   TableType != nullptr ? type.ptr() : nullptr, type.len());
}

SQLRETURN SQL_API SQLColumnsW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                              SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                              SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                              SQLSMALLINT NameLength3, SQLWCHAR* ColumnName,
                              SQLSMALLINT NameLength4) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  Utf8Arg column(ColumnName, NameLength4);
  return SQLColumns(StatementHandle, CatalogName != nullptr ? cat.ptr() : nullptr, cat.len(),
                    SchemaName != nullptr ? schema.ptr() : nullptr, schema.len(),
                    TableName != nullptr ? table.ptr() : nullptr, table.len(),
                    ColumnName != nullptr ? column.ptr() : nullptr, column.len());
}

SQLRETURN SQL_API SQLPrimaryKeysW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                  SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                  SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                                  SQLSMALLINT NameLength3) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  return SQLPrimaryKeys(StatementHandle, CatalogName != nullptr ? cat.ptr() : nullptr, cat.len(),
                        SchemaName != nullptr ? schema.ptr() : nullptr, schema.len(),
                        TableName != nullptr ? table.ptr() : nullptr, table.len());
}

SQLRETURN SQL_API SQLForeignKeysW(SQLHSTMT StatementHandle, SQLWCHAR* PKCatalogName,
                                  SQLSMALLINT NameLength1, SQLWCHAR* PKSchemaName,
                                  SQLSMALLINT NameLength2, SQLWCHAR* PKTableName,
                                  SQLSMALLINT NameLength3, SQLWCHAR* FKCatalogName,
                                  SQLSMALLINT NameLength4, SQLWCHAR* FKSchemaName,
                                  SQLSMALLINT NameLength5, SQLWCHAR* FKTableName,
                                  SQLSMALLINT NameLength6) {
  Utf8Arg a(PKCatalogName, NameLength1), b(PKSchemaName, NameLength2),
      c(PKTableName, NameLength3), d(FKCatalogName, NameLength4), e(FKSchemaName, NameLength5),
      f(FKTableName, NameLength6);
  return SQLForeignKeys(StatementHandle, a.ptr(), a.len(), b.ptr(), b.len(), c.ptr(), c.len(),
                        d.ptr(), d.len(), e.ptr(), e.len(), f.ptr(), f.len());
}

SQLRETURN SQL_API SQLStatisticsW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                 SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                 SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                                 SQLSMALLINT NameLength3, SQLUSMALLINT Unique,
                                 SQLUSMALLINT Reserved) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  return SQLStatistics(StatementHandle, cat.ptr(), cat.len(), schema.ptr(), schema.len(),
                       table.ptr(), table.len(), Unique, Reserved);
}

SQLRETURN SQL_API SQLSpecialColumnsW(SQLHSTMT StatementHandle, SQLUSMALLINT IdentifierType,
                                     SQLWCHAR* CatalogName, SQLSMALLINT NameLength1,
                                     SQLWCHAR* SchemaName, SQLSMALLINT NameLength2,
                                     SQLWCHAR* TableName, SQLSMALLINT NameLength3,
                                     SQLUSMALLINT Scope, SQLUSMALLINT Nullable) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  return SQLSpecialColumns(StatementHandle, IdentifierType, cat.ptr(), cat.len(), schema.ptr(),
                           schema.len(), table.ptr(), table.len(), Scope, Nullable);
}

SQLRETURN SQL_API SQLProceduresW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                 SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                 SQLSMALLINT NameLength2, SQLWCHAR* ProcName,
                                 SQLSMALLINT NameLength3) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg proc(ProcName, NameLength3);
  return SQLProcedures(StatementHandle, cat.ptr(), cat.len(), schema.ptr(), schema.len(),
                       proc.ptr(), proc.len());
}

SQLRETURN SQL_API SQLProcedureColumnsW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                       SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                       SQLSMALLINT NameLength2, SQLWCHAR* ProcName,
                                       SQLSMALLINT NameLength3, SQLWCHAR* ColumnName,
                                       SQLSMALLINT NameLength4) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg proc(ProcName, NameLength3);
  Utf8Arg column(ColumnName, NameLength4);
  return SQLProcedureColumns(StatementHandle, cat.ptr(), cat.len(), schema.ptr(), schema.len(),
                             proc.ptr(), proc.len(), column.ptr(), column.len());
}

SQLRETURN SQL_API SQLTablePrivilegesW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                      SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                      SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                                      SQLSMALLINT NameLength3) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  return SQLTablePrivileges(StatementHandle, cat.ptr(), cat.len(), schema.ptr(), schema.len(),
                            table.ptr(), table.len());
}

SQLRETURN SQL_API SQLColumnPrivilegesW(SQLHSTMT StatementHandle, SQLWCHAR* CatalogName,
                                       SQLSMALLINT NameLength1, SQLWCHAR* SchemaName,
                                       SQLSMALLINT NameLength2, SQLWCHAR* TableName,
                                       SQLSMALLINT NameLength3, SQLWCHAR* ColumnName,
                                       SQLSMALLINT NameLength4) {
  Utf8Arg cat(CatalogName, NameLength1);
  Utf8Arg schema(SchemaName, NameLength2);
  Utf8Arg table(TableName, NameLength3);
  Utf8Arg column(ColumnName, NameLength4);
  return SQLColumnPrivileges(StatementHandle, cat.ptr(), cat.len(), schema.ptr(), schema.len(),
                             table.ptr(), table.len(), column.ptr(), column.len());
}

SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT StatementHandle, SQLWCHAR* CursorName,
                                    SQLSMALLINT BufferLength, SQLSMALLINT* NameLengthPtr) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  return copyWideToBufferChars(stmt->diag, stmt->cursorName, CursorName, BufferLength,
                               NameLengthPtr);
}

SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT StatementHandle, SQLWCHAR* CursorName,
                                    SQLSMALLINT NameLength) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  stmt->cursorName = wideToUtf8(CursorName, NameLength);
  return SQL_SUCCESS;
}

}  // extern "C"
