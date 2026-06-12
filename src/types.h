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

#include <string>

#include <sql.h>
#include <sqlext.h>

namespace pinot_odbc {

struct Config;

// Pinot column data types as they appear in resultTable.dataSchema.columnDataTypes
// and in table schema fieldSpecs. Multi-value (array) columns are represented by
// base + isArray and surfaced through ODBC as JSON-encoded VARCHAR.
enum class PinotBaseType {
  Int,
  Long,
  Float,
  Double,
  BigDecimal,
  Boolean,
  Timestamp,
  String,
  Json,
  Bytes,
  Unknown,
};

struct PinotType {
  PinotBaseType base = PinotBaseType::Unknown;
  bool isArray = false;
};

PinotType pinotTypeFromString(const std::string& s);
std::string pinotTypeName(const PinotType& t);

// ODBC-facing metadata derived from a Pinot type.
struct SqlTypeMeta {
  SQLSMALLINT sqlType = SQL_VARCHAR;       // concise SQL type
  SQLSMALLINT sqlDataType = SQL_VARCHAR;   // verbose type (same except datetime)
  SQLSMALLINT datetimeSub = 0;             // SQL_DATETIME_SUB
  std::string typeName;                    // Pinot type name, e.g. "LONG"
  SQLULEN columnSize = 0;
  SQLSMALLINT decimalDigits = 0;
  SQLLEN displaySize = 0;
  SQLLEN octetLength = 0;
  bool isSigned = false;
  bool isNumeric = false;
  bool caseSensitive = false;
  int numPrecRadix = 0;                    // 0 means NULL
  std::string literalPrefix;
  std::string literalSuffix;
};

SqlTypeMeta sqlTypeMetaFor(const PinotType& t, long stringColumnSize);

// Default C type used for SQL_C_DEFAULT conversions of this Pinot type.
SQLSMALLINT defaultCTypeFor(const PinotType& t);

}  // namespace pinot_odbc
