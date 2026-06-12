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

#include "convert.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "api_helpers.h"
#include "types.h"
#include "unicode_conv.h"

namespace pinot_odbc {

namespace {

// Renders a JSON cell as the string an application sees for SQL_C_CHAR.
std::string renderCharValue(const nlohmann::json& cell, const Column& col) {
  if (cell.is_string()) return cell.get<std::string>();
  if (cell.is_boolean()) return cell.get<bool>() ? "1" : "0";
  if (cell.is_number_integer()) return std::to_string(cell.get<long long>());
  if (cell.is_number_unsigned()) return std::to_string(cell.get<unsigned long long>());
  if (cell.is_number_float()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", cell.get<double>());
    return buf;
  }
  (void)col;
  return cell.dump();  // arrays / objects -> JSON text
}

bool numericFromCell(const nlohmann::json& cell, double& out) {
  if (cell.is_number()) {
    out = cell.get<double>();
    return true;
  }
  if (cell.is_boolean()) {
    out = cell.get<bool>() ? 1.0 : 0.0;
    return true;
  }
  if (cell.is_string()) {
    const std::string& s = cell.get_ref<const std::string&>();
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE) return false;
    while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) end++;
    if (end != nullptr && *end != '\0') return false;
    out = v;
    return true;
  }
  return false;
}

bool integerFromCell(const nlohmann::json& cell, long long& out, bool& hadFraction) {
  hadFraction = false;
  if (cell.is_number_integer()) {
    out = cell.get<long long>();
    return true;
  }
  if (cell.is_number_unsigned()) {
    unsigned long long v = cell.get<unsigned long long>();
    if (v > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) return false;
    out = static_cast<long long>(v);
    return true;
  }
  if (cell.is_boolean()) {
    out = cell.get<bool>() ? 1 : 0;
    return true;
  }
  double d = 0;
  if (!numericFromCell(cell, d)) return false;
  if (std::isnan(d) || d < -9.3e18 || d > 9.3e18) return false;
  double truncated = std::trunc(d);
  hadFraction = (truncated != d);
  out = static_cast<long long>(truncated);
  return true;
}

// Writes an integral value with range checking. Returns 22003 via DiagError.
template <typename T>
SQLRETURN writeInteger(PinotStmt* stmt, long long v, bool hadFraction, SQLPOINTER target,
                       SQLLEN* indPtr) {
  if (v < static_cast<long long>(std::numeric_limits<T>::min()) ||
      v > static_cast<long long>(std::numeric_limits<T>::max())) {
    stmt->diag.add("22003", "Numeric value out of range");
    return SQL_ERROR;
  }
  if (target != nullptr) *static_cast<T*>(target) = static_cast<T>(v);
  if (indPtr != nullptr) *indPtr = sizeof(T);
  if (hadFraction) {
    stmt->diag.add("01S07", "Fractional truncation");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

template <typename T>
SQLRETURN writeUnsignedInteger(PinotStmt* stmt, long long v, bool hadFraction, SQLPOINTER target,
                               SQLLEN* indPtr) {
  if (v < 0 || static_cast<unsigned long long>(v) > std::numeric_limits<T>::max()) {
    stmt->diag.add("22003", "Numeric value out of range");
    return SQL_ERROR;
  }
  if (target != nullptr) *static_cast<T*>(target) = static_cast<T>(v);
  if (indPtr != nullptr) *indPtr = sizeof(T);
  if (hadFraction) {
    stmt->diag.add("01S07", "Fractional truncation");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

bool timestampFromCell(const nlohmann::json& cell, SQL_TIMESTAMP_STRUCT& ts) {
  if (cell.is_number_integer() || cell.is_number_unsigned()) {
    timestampFromEpochMillis(cell.get<long long>(), ts);
    return true;
  }
  if (cell.is_string()) {
    const std::string& s = cell.get_ref<const std::string&>();
    if (parseTimestampString(s, ts)) return true;
    // Epoch millis serialized as a string.
    errno = 0;
    char* end = nullptr;
    long long ms = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() && end != nullptr && *end == '\0' && errno != ERANGE) {
      timestampFromEpochMillis(ms, ts);
      return true;
    }
  }
  return false;
}

// Source bytes for SQL_C_BINARY targets.
std::vector<unsigned char> binaryFromCell(const nlohmann::json& cell, const Column& col,
                                          bool& ok) {
  ok = true;
  std::vector<unsigned char> bytes;
  if (cell.is_string()) {
    const std::string& s = cell.get_ref<const std::string&>();
    if (col.pinotType.base == PinotBaseType::Bytes && !col.pinotType.isArray) {
      if (hexDecode(s, bytes)) return bytes;
      // Fall through to raw bytes when the value is not valid hex.
    }
    bytes.assign(s.begin(), s.end());
    return bytes;
  }
  std::string rendered = renderCharValue(cell, col);
  bytes.assign(rendered.begin(), rendered.end());
  return bytes;
}

// Chunked copy of byte data into the target buffer. Used for CHAR (with NUL
// termination), WCHAR (unit = sizeof(SQLWCHAR)), and BINARY (no terminator).
SQLRETURN copyChunked(PinotStmt* stmt, SQLUSMALLINT col, const unsigned char* data,
                      size_t totalBytes, size_t unitSize, bool addTerminator, SQLPOINTER target,
                      SQLLEN bufLen, SQLLEN* indPtr, bool isGetData) {
  SQLLEN offset = 0;
  if (isGetData) {
    auto it = stmt->getDataOffsets.find(col);
    if (it != stmt->getDataOffsets.end()) {
      if (it->second < 0) return SQL_NO_DATA;  // already fully consumed
      offset = it->second;
    }
  }
  size_t remaining = totalBytes - static_cast<size_t>(offset);
  if (indPtr != nullptr) *indPtr = static_cast<SQLLEN>(remaining);

  if (target == nullptr || bufLen < 0) {
    if (target != nullptr) {
      stmt->diag.add("HY090", "Invalid string or buffer length");
      return SQL_ERROR;
    }
    // Length-only request: report size without consuming.
    return SQL_SUCCESS;
  }

  size_t capacity = static_cast<size_t>(bufLen);
  size_t terminatorBytes = addTerminator ? unitSize : 0;
  size_t usable = capacity > terminatorBytes ? capacity - terminatorBytes : 0;
  usable -= usable % unitSize;  // whole characters only
  size_t ncopy = std::min(remaining, usable);
  if (ncopy > 0) std::memcpy(target, data + offset, ncopy);
  if (addTerminator && capacity >= terminatorBytes) {
    std::memset(static_cast<unsigned char*>(target) + ncopy, 0, terminatorBytes);
  }

  if (ncopy < remaining) {
    if (isGetData) stmt->getDataOffsets[col] = offset + static_cast<SQLLEN>(ncopy);
    stmt->diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  if (isGetData) stmt->getDataOffsets[col] = -1;
  return SQL_SUCCESS;
}

}  // namespace

SQLRETURN convertCell(PinotStmt* stmt, SQLUSMALLINT col, SQLSMALLINT cType, SQLPOINTER target,
                      SQLLEN bufLen, SQLLEN* indPtr, bool isGetData) {
  const Column& column = stmt->rs.columns[col - 1];
  const nlohmann::json& row = stmt->rs.rows[static_cast<size_t>(stmt->cursor)];
  static const nlohmann::json kNull;
  const nlohmann::json& cell = (col - 1 < row.size()) ? row[col - 1] : kNull;

  if (cType == SQL_C_DEFAULT) cType = defaultCTypeFor(column.pinotType);

  // NULL handling.
  if (cell.is_null()) {
    if (isGetData) {
      auto it = stmt->getDataOffsets.find(col);
      if (it != stmt->getDataOffsets.end() && it->second < 0) return SQL_NO_DATA;
      stmt->getDataOffsets[col] = -1;
    }
    if (indPtr == nullptr) {
      stmt->diag.add("22002", "Indicator variable required but not supplied");
      return SQL_ERROR;
    }
    *indPtr = SQL_NULL_DATA;
    return SQL_SUCCESS;
  }

  switch (cType) {
    case SQL_C_CHAR: {
      std::string s = renderCharValue(cell, column);
      return copyChunked(stmt, col, reinterpret_cast<const unsigned char*>(s.data()), s.size(), 1,
                         /*addTerminator=*/true, target, bufLen, indPtr, isGetData);
    }
    case SQL_C_WCHAR: {
      WString w = utf8ToWide(renderCharValue(cell, column));
      return copyChunked(stmt, col, reinterpret_cast<const unsigned char*>(w.data()),
                         w.size() * sizeof(SQLWCHAR), sizeof(SQLWCHAR),
                         /*addTerminator=*/true, target, bufLen, indPtr, isGetData);
    }
    case SQL_C_BINARY: {
      bool ok = true;
      std::vector<unsigned char> bytes = binaryFromCell(cell, column, ok);
      if (!ok) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      return copyChunked(stmt, col, bytes.data(), bytes.size(), 1,
                         /*addTerminator=*/false, target, bufLen, indPtr, isGetData);
    }
    case SQL_C_BIT: {
      long long v = 0;
      bool frac = false;
      bool parsed = integerFromCell(cell, v, frac);
      if (!parsed && cell.is_string()) {
        std::string u = toUpperAscii(trimAscii(cell.get<std::string>()));
        if (u == "TRUE") { v = 1; parsed = true; }
        else if (u == "FALSE") { v = 0; parsed = true; }
      }
      if (!parsed) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      if (v != 0 && v != 1) {
        stmt->diag.add("22003", "Numeric value out of range");
        return SQL_ERROR;
      }
      if (target != nullptr) *static_cast<unsigned char*>(target) = static_cast<unsigned char>(v);
      if (indPtr != nullptr) *indPtr = 1;
      return SQL_SUCCESS;
    }
    case SQL_C_STINYINT:
    case SQL_C_TINYINT:
    case SQL_C_UTINYINT:
    case SQL_C_SSHORT:
    case SQL_C_SHORT:
    case SQL_C_USHORT:
    case SQL_C_SLONG:
    case SQL_C_LONG:
    case SQL_C_ULONG:
    case SQL_C_SBIGINT:
    case SQL_C_UBIGINT: {
      long long v = 0;
      bool frac = false;
      if (!integerFromCell(cell, v, frac)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      switch (cType) {
        case SQL_C_STINYINT:
        case SQL_C_TINYINT: return writeInteger<signed char>(stmt, v, frac, target, indPtr);
        case SQL_C_UTINYINT: return writeUnsignedInteger<unsigned char>(stmt, v, frac, target, indPtr);
        case SQL_C_SSHORT:
        case SQL_C_SHORT: return writeInteger<short>(stmt, v, frac, target, indPtr);
        case SQL_C_USHORT: return writeUnsignedInteger<unsigned short>(stmt, v, frac, target, indPtr);
        case SQL_C_SLONG:
        case SQL_C_LONG: return writeInteger<SQLINTEGER>(stmt, v, frac, target, indPtr);
        case SQL_C_ULONG: return writeUnsignedInteger<SQLUINTEGER>(stmt, v, frac, target, indPtr);
        case SQL_C_UBIGINT: return writeUnsignedInteger<unsigned long long>(stmt, v, frac, target, indPtr);
        default: {
          if (target != nullptr) *static_cast<long long*>(target) = v;
          if (indPtr != nullptr) *indPtr = sizeof(long long);
          if (frac) {
            stmt->diag.add("01S07", "Fractional truncation");
            return SQL_SUCCESS_WITH_INFO;
          }
          return SQL_SUCCESS;
        }
      }
    }
    case SQL_C_FLOAT: {
      double d = 0;
      if (!numericFromCell(cell, d)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      if (target != nullptr) *static_cast<float*>(target) = static_cast<float>(d);
      if (indPtr != nullptr) *indPtr = sizeof(float);
      return SQL_SUCCESS;
    }
    case SQL_C_DOUBLE: {
      double d = 0;
      if (!numericFromCell(cell, d)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      if (target != nullptr) *static_cast<double*>(target) = d;
      if (indPtr != nullptr) *indPtr = sizeof(double);
      return SQL_SUCCESS;
    }
    case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_TIMESTAMP: {
      SQL_TIMESTAMP_STRUCT ts;
      if (!timestampFromCell(cell, ts)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      if (target != nullptr) *static_cast<SQL_TIMESTAMP_STRUCT*>(target) = ts;
      if (indPtr != nullptr) *indPtr = sizeof(SQL_TIMESTAMP_STRUCT);
      return SQL_SUCCESS;
    }
    case SQL_C_TYPE_DATE:
    case SQL_C_DATE: {
      SQL_TIMESTAMP_STRUCT ts;
      if (!timestampFromCell(cell, ts)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      SQL_DATE_STRUCT d;
      d.year = ts.year;
      d.month = ts.month;
      d.day = ts.day;
      if (target != nullptr) *static_cast<SQL_DATE_STRUCT*>(target) = d;
      if (indPtr != nullptr) *indPtr = sizeof(SQL_DATE_STRUCT);
      return SQL_SUCCESS;
    }
    case SQL_C_TYPE_TIME:
    case SQL_C_TIME: {
      SQL_TIMESTAMP_STRUCT ts;
      if (!timestampFromCell(cell, ts)) {
        stmt->diag.add("22018", "Invalid character value for cast specification");
        return SQL_ERROR;
      }
      SQL_TIME_STRUCT t;
      t.hour = ts.hour;
      t.minute = ts.minute;
      t.second = ts.second;
      if (target != nullptr) *static_cast<SQL_TIME_STRUCT*>(target) = t;
      if (indPtr != nullptr) *indPtr = sizeof(SQL_TIME_STRUCT);
      return SQL_SUCCESS;
    }
    default:
      stmt->diag.add("07006", "Restricted data type attribute violation (unsupported C type " +
                                  std::to_string(cType) + ")");
      return SQL_ERROR;
  }
}

}  // namespace pinot_odbc
