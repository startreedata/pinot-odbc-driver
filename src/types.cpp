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

#include "types.h"

#include "config.h"

namespace pinot_odbc {

namespace {

PinotBaseType baseFromString(const std::string& s) {
  if (s == "INT") return PinotBaseType::Int;
  if (s == "LONG") return PinotBaseType::Long;
  if (s == "FLOAT") return PinotBaseType::Float;
  if (s == "DOUBLE") return PinotBaseType::Double;
  if (s == "BIG_DECIMAL") return PinotBaseType::BigDecimal;
  if (s == "BOOLEAN") return PinotBaseType::Boolean;
  if (s == "TIMESTAMP") return PinotBaseType::Timestamp;
  if (s == "STRING") return PinotBaseType::String;
  if (s == "JSON") return PinotBaseType::Json;
  if (s == "BYTES") return PinotBaseType::Bytes;
  return PinotBaseType::Unknown;
}

std::string baseName(PinotBaseType b) {
  switch (b) {
    case PinotBaseType::Int: return "INT";
    case PinotBaseType::Long: return "LONG";
    case PinotBaseType::Float: return "FLOAT";
    case PinotBaseType::Double: return "DOUBLE";
    case PinotBaseType::BigDecimal: return "BIG_DECIMAL";
    case PinotBaseType::Boolean: return "BOOLEAN";
    case PinotBaseType::Timestamp: return "TIMESTAMP";
    case PinotBaseType::String: return "STRING";
    case PinotBaseType::Json: return "JSON";
    case PinotBaseType::Bytes: return "BYTES";
    case PinotBaseType::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace

PinotType pinotTypeFromString(const std::string& s) {
  std::string u = toUpperAscii(trimAscii(s));
  PinotType t;
  static const char kArraySuffix[] = "_ARRAY";
  const size_t suffixLen = sizeof(kArraySuffix) - 1;
  if (u.size() > suffixLen && u.compare(u.size() - suffixLen, suffixLen, kArraySuffix) == 0) {
    t.isArray = true;
    u = u.substr(0, u.size() - suffixLen);
  }
  t.base = baseFromString(u);
  return t;
}

std::string pinotTypeName(const PinotType& t) {
  std::string n = baseName(t.base);
  if (t.isArray) n += "_ARRAY";
  return n;
}

SqlTypeMeta sqlTypeMetaFor(const PinotType& t, long stringColumnSize) {
  SqlTypeMeta m;
  m.typeName = pinotTypeName(t);

  if (t.isArray) {
    // Multi-value columns are surfaced as a JSON-encoded string.
    m.sqlType = m.sqlDataType = SQL_VARCHAR;
    m.columnSize = static_cast<SQLULEN>(stringColumnSize);
    m.displaySize = stringColumnSize;
    m.octetLength = stringColumnSize;
    m.caseSensitive = true;
    m.literalPrefix = "'";
    m.literalSuffix = "'";
    return m;
  }

  switch (t.base) {
    case PinotBaseType::Int:
      m.sqlType = m.sqlDataType = SQL_INTEGER;
      m.columnSize = 10;
      m.displaySize = 11;
      m.octetLength = 4;
      m.isSigned = true;
      m.isNumeric = true;
      m.numPrecRadix = 10;
      break;
    case PinotBaseType::Long:
      m.sqlType = m.sqlDataType = SQL_BIGINT;
      m.columnSize = 19;
      m.displaySize = 20;
      m.octetLength = 8;
      m.isSigned = true;
      m.isNumeric = true;
      m.numPrecRadix = 10;
      break;
    case PinotBaseType::Float:
      m.sqlType = m.sqlDataType = SQL_REAL;
      m.columnSize = 7;
      m.displaySize = 14;
      m.octetLength = 4;
      m.isSigned = true;
      m.isNumeric = true;
      m.numPrecRadix = 10;
      break;
    case PinotBaseType::Double:
      m.sqlType = m.sqlDataType = SQL_DOUBLE;
      m.columnSize = 15;
      m.displaySize = 24;
      m.octetLength = 8;
      m.isSigned = true;
      m.isNumeric = true;
      m.numPrecRadix = 10;
      break;
    case PinotBaseType::BigDecimal:
      m.sqlType = m.sqlDataType = SQL_DECIMAL;
      m.columnSize = 38;
      m.decimalDigits = 18;
      m.displaySize = 40;
      m.octetLength = 40;
      m.isSigned = true;
      m.isNumeric = true;
      m.numPrecRadix = 10;
      break;
    case PinotBaseType::Boolean:
      m.sqlType = m.sqlDataType = SQL_BIT;
      m.columnSize = 1;
      m.displaySize = 1;
      m.octetLength = 1;
      break;
    case PinotBaseType::Timestamp:
      m.sqlType = SQL_TYPE_TIMESTAMP;
      m.sqlDataType = SQL_DATETIME;
      m.datetimeSub = SQL_CODE_TIMESTAMP;
      m.columnSize = 23;  // yyyy-MM-dd HH:mm:ss.fff
      m.decimalDigits = 3;
      m.displaySize = 23;
      m.octetLength = static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT));
      m.literalPrefix = "'";
      m.literalSuffix = "'";
      break;
    case PinotBaseType::String:
    case PinotBaseType::Unknown:
      m.sqlType = m.sqlDataType = SQL_VARCHAR;
      m.columnSize = static_cast<SQLULEN>(stringColumnSize);
      m.displaySize = stringColumnSize;
      m.octetLength = stringColumnSize;
      m.caseSensitive = true;
      m.literalPrefix = "'";
      m.literalSuffix = "'";
      break;
    case PinotBaseType::Json:
      m.sqlType = m.sqlDataType = SQL_LONGVARCHAR;
      m.columnSize = 2147483647;
      m.displaySize = 2147483647;
      m.octetLength = 2147483647;
      m.caseSensitive = true;
      m.literalPrefix = "'";
      m.literalSuffix = "'";
      break;
    case PinotBaseType::Bytes:
      m.sqlType = m.sqlDataType = SQL_VARBINARY;
      m.columnSize = static_cast<SQLULEN>(stringColumnSize);
      m.displaySize = stringColumnSize * 2;
      m.octetLength = stringColumnSize;
      m.literalPrefix = "'";
      m.literalSuffix = "'";
      break;
  }
  return m;
}

SQLSMALLINT defaultCTypeFor(const PinotType& t) {
  if (t.isArray) return SQL_C_CHAR;
  switch (t.base) {
    case PinotBaseType::Int: return SQL_C_SLONG;
    case PinotBaseType::Long: return SQL_C_SBIGINT;
    case PinotBaseType::Float: return SQL_C_FLOAT;
    case PinotBaseType::Double: return SQL_C_DOUBLE;
    case PinotBaseType::Boolean: return SQL_C_BIT;
    case PinotBaseType::Timestamp: return SQL_C_TYPE_TIMESTAMP;
    case PinotBaseType::Bytes: return SQL_C_BINARY;
    case PinotBaseType::BigDecimal:
    case PinotBaseType::String:
    case PinotBaseType::Json:
    case PinotBaseType::Unknown:
      return SQL_C_CHAR;
  }
  return SQL_C_CHAR;
}

}  // namespace pinot_odbc
