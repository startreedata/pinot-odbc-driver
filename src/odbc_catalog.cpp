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

// Catalog functions: SQLTables, SQLColumns, SQLPrimaryKeys, SQLGetTypeInfo,
// and empty result sets for the rest of the catalog API.

#include <sql.h>
#include <sqlext.h>

#include "api_helpers.h"
#include "handles.h"

using namespace pinot_odbc;

namespace {

using ColumnSpec = std::vector<std::pair<std::string, PinotBaseType>>;

const PinotBaseType S = PinotBaseType::String;
const PinotBaseType I = PinotBaseType::Int;

void installResultSet(PinotStmt* stmt, ResultSet rs) {
  stmt->rs = std::move(rs);
  stmt->hasResult = true;
  stmt->cursor = -1;
  stmt->getDataOffsets.clear();
}

// Collects (column name, pinot type, ordinal) triples from a Pinot schema.
struct SchemaField {
  std::string name;
  PinotType type;
};

std::vector<SchemaField> fieldsFromSchema(const nlohmann::json& schema) {
  std::vector<SchemaField> fields;
  auto collect = [&fields](const nlohmann::json& specs) {
    if (!specs.is_array()) return;
    for (const auto& spec : specs) {
      if (!spec.is_object() || !spec.contains("name")) continue;
      SchemaField f;
      f.name = spec["name"].get<std::string>();
      std::string dataType = spec.value("dataType", "STRING");
      f.type = pinotTypeFromString(dataType);
      if (spec.contains("singleValueField") && spec["singleValueField"].is_boolean() &&
          !spec["singleValueField"].get<bool>()) {
        f.type.isArray = true;
      }
      fields.push_back(std::move(f));
    }
  };
  collect(schema.value("dimensionFieldSpecs", nlohmann::json::array()));
  collect(schema.value("metricFieldSpecs", nlohmann::json::array()));
  collect(schema.value("dateTimeFieldSpecs", nlohmann::json::array()));
  return fields;
}

SQLRETURN catalogError(PinotStmt* stmt, const DiagError& e) {
  stmt->diag.add(e.sqlstate, e.message, e.native);
  return SQL_ERROR;
}

}  // namespace

extern "C" {

SQLRETURN SQL_API SQLTables(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                            SQLSMALLINT NameLength1, SQLCHAR* SchemaName, SQLSMALLINT NameLength2,
                            SQLCHAR* TableName, SQLSMALLINT NameLength3, SQLCHAR* TableType,
                            SQLSMALLINT NameLength4) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();

  (void)CatalogName;
  (void)NameLength1;
  (void)SchemaName;
  (void)NameLength2;
  std::string tablePattern = fromSqlString(TableName, NameLength3);
  std::string typeFilter = fromSqlString(TableType, NameLength4);

  static const ColumnSpec kCols = {{"TABLE_CAT", S},
                                   {"TABLE_SCHEM", S},
                                   {"TABLE_NAME", S},
                                   {"TABLE_TYPE", S},
                                   {"REMARKS", S}};
  ResultSet rs = makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize);

  // Type filter: include rows only when "TABLE" is requested (or no filter).
  bool wantsTables = typeFilter.empty();
  if (!wantsTables) {
    std::string upper = toUpperAscii(typeFilter);
    wantsTables = upper.find("TABLE") != std::string::npos || upper == "%";
  }

  if (wantsTables) {
    try {
      std::vector<std::string> tables = stmt->dbc->client->listTables();
      for (const std::string& t : tables) {
        if (!likeMatch(tablePattern, t)) continue;
        rs.rows.push_back({nullptr, nullptr, t, "TABLE", nullptr});
      }
    } catch (const DiagError& e) {
      return catalogError(stmt, e);
    }
  }
  installResultSet(stmt, std::move(rs));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                             SQLSMALLINT NameLength1, SQLCHAR* SchemaName, SQLSMALLINT NameLength2,
                             SQLCHAR* TableName, SQLSMALLINT NameLength3, SQLCHAR* ColumnName,
                             SQLSMALLINT NameLength4) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();

  (void)CatalogName;
  (void)NameLength1;
  (void)SchemaName;
  (void)NameLength2;
  std::string tablePattern = fromSqlString(TableName, NameLength3);
  std::string columnPattern = fromSqlString(ColumnName, NameLength4);

  static const ColumnSpec kCols = {
      {"TABLE_CAT", S},        {"TABLE_SCHEM", S},      {"TABLE_NAME", S},
      {"COLUMN_NAME", S},      {"DATA_TYPE", I},        {"TYPE_NAME", S},
      {"COLUMN_SIZE", I},      {"BUFFER_LENGTH", I},    {"DECIMAL_DIGITS", I},
      {"NUM_PREC_RADIX", I},   {"NULLABLE", I},         {"REMARKS", S},
      {"COLUMN_DEF", S},       {"SQL_DATA_TYPE", I},    {"SQL_DATETIME_SUB", I},
      {"CHAR_OCTET_LENGTH", I}, {"ORDINAL_POSITION", I}, {"IS_NULLABLE", S}};
  ResultSet rs = makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize);

  try {
    std::vector<std::string> tables = stmt->dbc->client->listTables();
    for (const std::string& table : tables) {
      if (!likeMatch(tablePattern, table)) continue;
      nlohmann::json schema = stmt->dbc->client->tableSchema(table);
      std::vector<SchemaField> fields = fieldsFromSchema(schema);
      int ordinal = 0;
      for (const SchemaField& f : fields) {
        ordinal++;
        if (!likeMatch(columnPattern, f.name)) continue;
        SqlTypeMeta meta = sqlTypeMetaFor(f.type, stmt->dbc->cfg.stringColumnSize);
        nlohmann::json row = nlohmann::json::array();
        row.push_back(nullptr);                                    // TABLE_CAT
        row.push_back(nullptr);                                    // TABLE_SCHEM
        row.push_back(table);                                      // TABLE_NAME
        row.push_back(f.name);                                     // COLUMN_NAME
        row.push_back(meta.sqlType);                               // DATA_TYPE
        row.push_back(meta.typeName);                              // TYPE_NAME
        row.push_back(static_cast<long long>(meta.columnSize));    // COLUMN_SIZE
        row.push_back(static_cast<long long>(meta.octetLength));   // BUFFER_LENGTH
        if (meta.isNumeric) row.push_back(meta.decimalDigits);     // DECIMAL_DIGITS
        else row.push_back(nullptr);
        if (meta.numPrecRadix != 0) row.push_back(meta.numPrecRadix);  // NUM_PREC_RADIX
        else row.push_back(nullptr);
        row.push_back(SQL_NULLABLE);                               // NULLABLE
        row.push_back(nullptr);                                    // REMARKS
        row.push_back(nullptr);                                    // COLUMN_DEF
        row.push_back(meta.sqlDataType);                           // SQL_DATA_TYPE
        if (meta.datetimeSub != 0) row.push_back(meta.datetimeSub);  // SQL_DATETIME_SUB
        else row.push_back(nullptr);
        row.push_back(static_cast<long long>(meta.octetLength));   // CHAR_OCTET_LENGTH
        row.push_back(ordinal);                                    // ORDINAL_POSITION
        row.push_back("YES");                                      // IS_NULLABLE
        rs.rows.push_back(std::move(row));
      }
    }
  } catch (const DiagError& e) {
    return catalogError(stmt, e);
  }
  installResultSet(stmt, std::move(rs));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                 SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                 SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                 SQLSMALLINT NameLength3) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();

  (void)CatalogName;
  (void)NameLength1;
  (void)SchemaName;
  (void)NameLength2;
  std::string table = fromSqlString(TableName, NameLength3);

  static const ColumnSpec kCols = {{"TABLE_CAT", S},  {"TABLE_SCHEM", S}, {"TABLE_NAME", S},
                                   {"COLUMN_NAME", S}, {"KEY_SEQ", I},     {"PK_NAME", S}};
  ResultSet rs = makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize);

  if (!table.empty()) {
    try {
      nlohmann::json schema = stmt->dbc->client->tableSchema(table);
      if (schema.contains("primaryKeyColumns") && schema["primaryKeyColumns"].is_array()) {
        int seq = 0;
        for (const auto& pk : schema["primaryKeyColumns"]) {
          if (!pk.is_string()) continue;
          seq++;
          rs.rows.push_back(
              {nullptr, nullptr, table, pk.get<std::string>(), seq, nullptr});
        }
      }
    } catch (const DiagError& e) {
      return catalogError(stmt, e);
    }
  }
  installResultSet(stmt, std::move(rs));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT StatementHandle, SQLSMALLINT DataType) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();

  static const ColumnSpec kCols = {
      {"TYPE_NAME", S},          {"DATA_TYPE", I},      {"COLUMN_SIZE", I},
      {"LITERAL_PREFIX", S},     {"LITERAL_SUFFIX", S}, {"CREATE_PARAMS", S},
      {"NULLABLE", I},           {"CASE_SENSITIVE", I}, {"SEARCHABLE", I},
      {"UNSIGNED_ATTRIBUTE", I}, {"FIXED_PREC_SCALE", I}, {"AUTO_UNIQUE_VALUE", I},
      {"LOCAL_TYPE_NAME", S},    {"MINIMUM_SCALE", I},  {"MAXIMUM_SCALE", I},
      {"SQL_DATA_TYPE", I},      {"SQL_DATETIME_SUB", I}, {"NUM_PREC_RADIX", I},
      {"INTERVAL_PRECISION", I}};
  ResultSet rs = makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize);

  static const PinotBaseType kTypes[] = {
      PinotBaseType::Boolean,   PinotBaseType::Int,    PinotBaseType::Long,
      PinotBaseType::Float,     PinotBaseType::Double, PinotBaseType::BigDecimal,
      PinotBaseType::Timestamp, PinotBaseType::String, PinotBaseType::Json,
      PinotBaseType::Bytes};
  for (PinotBaseType base : kTypes) {
    PinotType t;
    t.base = base;
    SqlTypeMeta meta = sqlTypeMetaFor(t, stmt->dbc->cfg.stringColumnSize);
    if (DataType != SQL_ALL_TYPES && meta.sqlType != DataType) continue;
    nlohmann::json row = nlohmann::json::array();
    row.push_back(meta.typeName);                                 // TYPE_NAME
    row.push_back(meta.sqlType);                                  // DATA_TYPE
    row.push_back(static_cast<long long>(meta.columnSize));       // COLUMN_SIZE
    if (meta.literalPrefix.empty()) row.push_back(nullptr);       // LITERAL_PREFIX
    else row.push_back(meta.literalPrefix);
    if (meta.literalSuffix.empty()) row.push_back(nullptr);       // LITERAL_SUFFIX
    else row.push_back(meta.literalSuffix);
    row.push_back(nullptr);                                       // CREATE_PARAMS
    row.push_back(SQL_NULLABLE);                                  // NULLABLE
    row.push_back(meta.caseSensitive ? SQL_TRUE : SQL_FALSE);     // CASE_SENSITIVE
    row.push_back(SQL_SEARCHABLE);                                // SEARCHABLE
    if (meta.isNumeric) row.push_back(meta.isSigned ? SQL_FALSE : SQL_TRUE);
    else row.push_back(nullptr);                                  // UNSIGNED_ATTRIBUTE
    row.push_back(SQL_FALSE);                                     // FIXED_PREC_SCALE
    if (meta.isNumeric) row.push_back(SQL_FALSE);                 // AUTO_UNIQUE_VALUE
    else row.push_back(nullptr);
    row.push_back(meta.typeName);                                 // LOCAL_TYPE_NAME
    row.push_back(meta.decimalDigits);                            // MINIMUM_SCALE
    row.push_back(meta.decimalDigits);                            // MAXIMUM_SCALE
    row.push_back(meta.sqlDataType);                              // SQL_DATA_TYPE
    if (meta.datetimeSub != 0) row.push_back(meta.datetimeSub);   // SQL_DATETIME_SUB
    else row.push_back(nullptr);
    if (meta.numPrecRadix != 0) row.push_back(meta.numPrecRadix); // NUM_PREC_RADIX
    else row.push_back(nullptr);
    row.push_back(nullptr);                                       // INTERVAL_PRECISION
    rs.rows.push_back(std::move(row));
  }
  installResultSet(stmt, std::move(rs));
  return SQL_SUCCESS;
}

// ---- catalog functions that legitimately return empty result sets ----

SQLRETURN SQL_API SQLStatistics(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                SQLSMALLINT NameLength3, SQLUSMALLINT Unique,
                                SQLUSMALLINT Reserved) {
  (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
  (void)TableName; (void)NameLength3; (void)Unique; (void)Reserved;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {
      {"TABLE_CAT", S},   {"TABLE_SCHEM", S}, {"TABLE_NAME", S},  {"NON_UNIQUE", I},
      {"INDEX_QUALIFIER", S}, {"INDEX_NAME", S}, {"TYPE", I},     {"ORDINAL_POSITION", I},
      {"COLUMN_NAME", S}, {"ASC_OR_DESC", S}, {"CARDINALITY", I}, {"PAGES", I},
      {"FILTER_CONDITION", S}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT StatementHandle, SQLUSMALLINT IdentifierType,
                                    SQLCHAR* CatalogName, SQLSMALLINT NameLength1,
                                    SQLCHAR* SchemaName, SQLSMALLINT NameLength2,
                                    SQLCHAR* TableName, SQLSMALLINT NameLength3,
                                    SQLUSMALLINT Scope, SQLUSMALLINT Nullable) {
  (void)IdentifierType; (void)CatalogName; (void)NameLength1; (void)SchemaName;
  (void)NameLength2; (void)TableName; (void)NameLength3; (void)Scope; (void)Nullable;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {{"SCOPE", I},          {"COLUMN_NAME", S},
                                   {"DATA_TYPE", I},      {"TYPE_NAME", S},
                                   {"COLUMN_SIZE", I},    {"BUFFER_LENGTH", I},
                                   {"DECIMAL_DIGITS", I}, {"PSEUDO_COLUMN", I}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLForeignKeys(SQLHSTMT StatementHandle, SQLCHAR* PKCatalogName,
                                 SQLSMALLINT NameLength1, SQLCHAR* PKSchemaName,
                                 SQLSMALLINT NameLength2, SQLCHAR* PKTableName,
                                 SQLSMALLINT NameLength3, SQLCHAR* FKCatalogName,
                                 SQLSMALLINT NameLength4, SQLCHAR* FKSchemaName,
                                 SQLSMALLINT NameLength5, SQLCHAR* FKTableName,
                                 SQLSMALLINT NameLength6) {
  (void)PKCatalogName; (void)NameLength1; (void)PKSchemaName; (void)NameLength2;
  (void)PKTableName; (void)NameLength3; (void)FKCatalogName; (void)NameLength4;
  (void)FKSchemaName; (void)NameLength5; (void)FKTableName; (void)NameLength6;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {
      {"PKTABLE_CAT", S},  {"PKTABLE_SCHEM", S}, {"PKTABLE_NAME", S}, {"PKCOLUMN_NAME", S},
      {"FKTABLE_CAT", S},  {"FKTABLE_SCHEM", S}, {"FKTABLE_NAME", S}, {"FKCOLUMN_NAME", S},
      {"KEY_SEQ", I},      {"UPDATE_RULE", I},   {"DELETE_RULE", I},  {"FK_NAME", S},
      {"PK_NAME", S},      {"DEFERRABILITY", I}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLProcedures(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                SQLSMALLINT NameLength2, SQLCHAR* ProcName,
                                SQLSMALLINT NameLength3) {
  (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
  (void)ProcName; (void)NameLength3;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {
      {"PROCEDURE_CAT", S},  {"PROCEDURE_SCHEM", S}, {"PROCEDURE_NAME", S},
      {"NUM_INPUT_PARAMS", I}, {"NUM_OUTPUT_PARAMS", I}, {"NUM_RESULT_SETS", I},
      {"REMARKS", S},        {"PROCEDURE_TYPE", I}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLProcedureColumns(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                      SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                      SQLSMALLINT NameLength2, SQLCHAR* ProcName,
                                      SQLSMALLINT NameLength3, SQLCHAR* ColumnName,
                                      SQLSMALLINT NameLength4) {
  (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
  (void)ProcName; (void)NameLength3; (void)ColumnName; (void)NameLength4;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {
      {"PROCEDURE_CAT", S}, {"PROCEDURE_SCHEM", S}, {"PROCEDURE_NAME", S},
      {"COLUMN_NAME", S},   {"COLUMN_TYPE", I},     {"DATA_TYPE", I},
      {"TYPE_NAME", S},     {"COLUMN_SIZE", I},     {"BUFFER_LENGTH", I},
      {"DECIMAL_DIGITS", I}, {"NUM_PREC_RADIX", I}, {"NULLABLE", I},
      {"REMARKS", S},       {"COLUMN_DEF", S},      {"SQL_DATA_TYPE", I},
      {"SQL_DATETIME_SUB", I}, {"CHAR_OCTET_LENGTH", I}, {"ORDINAL_POSITION", I},
      {"IS_NULLABLE", S}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTablePrivileges(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                     SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                     SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                     SQLSMALLINT NameLength3) {
  (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
  (void)TableName; (void)NameLength3;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {{"TABLE_CAT", S}, {"TABLE_SCHEM", S}, {"TABLE_NAME", S},
                                   {"GRANTOR", S},   {"GRANTEE", S},     {"PRIVILEGE", S},
                                   {"IS_GRANTABLE", S}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumnPrivileges(SQLHSTMT StatementHandle, SQLCHAR* CatalogName,
                                      SQLSMALLINT NameLength1, SQLCHAR* SchemaName,
                                      SQLSMALLINT NameLength2, SQLCHAR* TableName,
                                      SQLSMALLINT NameLength3, SQLCHAR* ColumnName,
                                      SQLSMALLINT NameLength4) {
  (void)CatalogName; (void)NameLength1; (void)SchemaName; (void)NameLength2;
  (void)TableName; (void)NameLength3; (void)ColumnName; (void)NameLength4;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  static const ColumnSpec kCols = {{"TABLE_CAT", S}, {"TABLE_SCHEM", S}, {"TABLE_NAME", S},
                                   {"COLUMN_NAME", S}, {"GRANTOR", S},   {"GRANTEE", S},
                                   {"PRIVILEGE", S},   {"IS_GRANTABLE", S}};
  installResultSet(stmt, makeCatalogResultSet(kCols, stmt->dbc->cfg.stringColumnSize));
  return SQL_SUCCESS;
}

}  // extern "C"
