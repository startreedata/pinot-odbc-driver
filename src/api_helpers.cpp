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

#include "api_helpers.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace pinot_odbc {

bool likeMatch(const std::string& pattern, const std::string& value) {
  if (pattern.empty()) return true;
  // Iterative wildcard match with backtracking on '%'.
  size_t p = 0, v = 0;
  size_t starP = std::string::npos, starV = 0;
  const size_t pn = pattern.size(), vn = value.size();
  while (v < vn) {
    bool matched = false;
    if (p < pn) {
      char pc = pattern[p];
      if (pc == '%') {
        starP = p++;
        starV = v;
        continue;
      }
      if (pc == '\\' && p + 1 < pn) {
        if (pattern[p + 1] == value[v]) {
          p += 2;
          v++;
          matched = true;
        }
      } else if (pc == '_' || pc == value[v]) {
        p++;
        v++;
        matched = true;
      }
    }
    if (!matched) {
      if (starP == std::string::npos) return false;
      p = starP + 1;
      v = ++starV;
    }
  }
  while (p < pn && pattern[p] == '%') p++;
  return p == pn;
}

bool parseTimestampString(const std::string& s, SQL_TIMESTAMP_STRUCT& ts) {
  std::memset(&ts, 0, sizeof(ts));
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int consumed = 0;
  // Accept "YYYY-MM-DD[ T]HH:MM:SS[.fraction]" or "YYYY-MM-DD".
  if (std::sscanf(s.c_str(), "%d-%d-%d%n", &year, &month, &day, &consumed) != 3) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  size_t pos = static_cast<size_t>(consumed);
  unsigned fractionNs = 0;
  if (pos < s.size() && (s[pos] == ' ' || s[pos] == 'T')) {
    pos++;
    int timeConsumed = 0;
    if (std::sscanf(s.c_str() + pos, "%d:%d:%d%n", &hour, &minute, &second, &timeConsumed) != 3) {
      return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
      return false;
    }
    pos += static_cast<size_t>(timeConsumed);
    if (pos < s.size() && s[pos] == '.') {
      pos++;
      unsigned digits = 0;
      unsigned value = 0;
      while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) && digits < 9) {
        value = value * 10 + static_cast<unsigned>(s[pos] - '0');
        digits++;
        pos++;
      }
      while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
      for (unsigned i = digits; i < 9; i++) value *= 10;
      fractionNs = value;
    }
  }
  ts.year = static_cast<SQLSMALLINT>(year);
  ts.month = static_cast<SQLUSMALLINT>(month);
  ts.day = static_cast<SQLUSMALLINT>(day);
  ts.hour = static_cast<SQLUSMALLINT>(hour);
  ts.minute = static_cast<SQLUSMALLINT>(minute);
  ts.second = static_cast<SQLUSMALLINT>(second);
  ts.fraction = fractionNs;
  return true;
}

void timestampFromEpochMillis(long long ms, SQL_TIMESTAMP_STRUCT& ts) {
  std::memset(&ts, 0, sizeof(ts));
  time_t secs = static_cast<time_t>(ms / 1000);
  long long rem = ms % 1000;
  if (rem < 0) {
    rem += 1000;
    secs -= 1;
  }
  struct tm tmv;
  gmtime_r(&secs, &tmv);
  ts.year = static_cast<SQLSMALLINT>(tmv.tm_year + 1900);
  ts.month = static_cast<SQLUSMALLINT>(tmv.tm_mon + 1);
  ts.day = static_cast<SQLUSMALLINT>(tmv.tm_mday);
  ts.hour = static_cast<SQLUSMALLINT>(tmv.tm_hour);
  ts.minute = static_cast<SQLUSMALLINT>(tmv.tm_min);
  ts.second = static_cast<SQLUSMALLINT>(tmv.tm_sec);
  ts.fraction = static_cast<SQLUINTEGER>(rem * 1000000);  // ns
}

std::string formatTimestamp(const SQL_TIMESTAMP_STRUCT& ts) {
  char buf[64];
  unsigned millis = ts.fraction / 1000000;
  if (millis > 0) {
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02u:%02u:%02u.%03u", ts.year, ts.month,
                  ts.day, ts.hour, ts.minute, ts.second, millis);
  } else {
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02u:%02u:%02u", ts.year, ts.month, ts.day,
                  ts.hour, ts.minute, ts.second);
  }
  return buf;
}

namespace {

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool hexDecode(const std::string& hex, std::vector<unsigned char>& out) {
  out.clear();
  if (hex.size() % 2 != 0) return false;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    int hi = hexNibble(hex[i]);
    int lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

std::string hexEncode(const unsigned char* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += kHex[data[i] >> 4];
    out += kHex[data[i] & 0xF];
  }
  return out;
}

// ---- SQL text scanning -------------------------------------------------------

namespace {

// Calls fn(position) for each '?' parameter marker outside string literals,
// quoted identifiers, and comments.
template <typename Fn>
void scanParameterMarkers(const std::string& sql, Fn fn) {
  const size_t n = sql.size();
  size_t i = 0;
  while (i < n) {
    char c = sql[i];
    if (c == '\'') {
      i++;
      while (i < n) {
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            i += 2;
            continue;
          }
          i++;
          break;
        }
        i++;
      }
    } else if (c == '"') {
      i++;
      while (i < n && sql[i] != '"') i++;
      if (i < n) i++;
    } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') i++;
    } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) i++;
      i = (i + 1 < n) ? i + 2 : n;
    } else {
      if (c == '?') fn(i);
      i++;
    }
  }
}

std::string escapeSqlString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (char c : s) {
    if (c == '\'') out += "''";
    else out += c;
  }
  out += '\'';
  return out;
}

std::string renderDouble(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return buf;
}

// Renders the bound parameter's current value as a SQL literal.
std::string renderParameter(const ParamBinding& p, int oneBasedIndex) {
  SQLLEN ind = 0;
  if (p.indPtr != nullptr) {
    ind = *p.indPtr;
  } else if (p.cType == SQL_C_CHAR || p.cType == SQL_C_WCHAR || p.cType == SQL_C_BINARY) {
    ind = SQL_NTS;
  }
  if (ind == SQL_NULL_DATA) return "NULL";
  if (p.ptr == nullptr) {
    throw DiagError{"07002", "Parameter " + std::to_string(oneBasedIndex) + " has no value"};
  }
  if (ind <= SQL_LEN_DATA_AT_EXEC_OFFSET || ind == SQL_DATA_AT_EXEC) {
    throw DiagError{"HYC00", "Data-at-execution parameters are not supported"};
  }

  SQLSMALLINT cType = p.cType;
  if (cType == SQL_C_DEFAULT) {
    switch (p.sqlType) {
      case SQL_INTEGER: cType = SQL_C_SLONG; break;
      case SQL_SMALLINT: cType = SQL_C_SSHORT; break;
      case SQL_TINYINT: cType = SQL_C_STINYINT; break;
      case SQL_BIGINT: cType = SQL_C_SBIGINT; break;
      case SQL_REAL: cType = SQL_C_FLOAT; break;
      case SQL_FLOAT:
      case SQL_DOUBLE: cType = SQL_C_DOUBLE; break;
      case SQL_BIT: cType = SQL_C_BIT; break;
      case SQL_TYPE_TIMESTAMP: cType = SQL_C_TYPE_TIMESTAMP; break;
      case SQL_TYPE_DATE: cType = SQL_C_TYPE_DATE; break;
      case SQL_BINARY:
      case SQL_VARBINARY:
      case SQL_LONGVARBINARY: cType = SQL_C_BINARY; break;
      default: cType = SQL_C_CHAR; break;
    }
  }

  switch (cType) {
    case SQL_C_CHAR: {
      const char* s = static_cast<const char*>(p.ptr);
      size_t len = (ind == SQL_NTS) ? std::strlen(s) : static_cast<size_t>(ind);
      return escapeSqlString(std::string(s, len));
    }
    case SQL_C_WCHAR: {
      const SQLWCHAR* s = static_cast<const SQLWCHAR*>(p.ptr);
      SQLINTEGER lenChars =
          (ind == SQL_NTS) ? SQL_NTS
                           : static_cast<SQLINTEGER>(ind / static_cast<SQLLEN>(sizeof(SQLWCHAR)));
      return escapeSqlString(wideToUtf8(s, lenChars));
    }
    case SQL_C_BIT: {
      unsigned char v = *static_cast<unsigned char*>(p.ptr);
      return v ? "true" : "false";
    }
    case SQL_C_STINYINT:
    case SQL_C_TINYINT:
      return std::to_string(static_cast<int>(*static_cast<signed char*>(p.ptr)));
    case SQL_C_UTINYINT:
      return std::to_string(static_cast<unsigned>(*static_cast<unsigned char*>(p.ptr)));
    case SQL_C_SSHORT:
    case SQL_C_SHORT:
      return std::to_string(*static_cast<short*>(p.ptr));
    case SQL_C_USHORT:
      return std::to_string(*static_cast<unsigned short*>(p.ptr));
    case SQL_C_SLONG:
    case SQL_C_LONG:
      return std::to_string(*static_cast<SQLINTEGER*>(p.ptr));
    case SQL_C_ULONG:
      return std::to_string(*static_cast<SQLUINTEGER*>(p.ptr));
    case SQL_C_SBIGINT:
      return std::to_string(*static_cast<long long*>(p.ptr));
    case SQL_C_UBIGINT:
      return std::to_string(*static_cast<unsigned long long*>(p.ptr));
    case SQL_C_FLOAT:
      return renderDouble(*static_cast<float*>(p.ptr));
    case SQL_C_DOUBLE:
      return renderDouble(*static_cast<double*>(p.ptr));
    case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_TIMESTAMP: {
      const auto* ts = static_cast<SQL_TIMESTAMP_STRUCT*>(p.ptr);
      return "'" + formatTimestamp(*ts) + "'";
    }
    case SQL_C_TYPE_DATE:
    case SQL_C_DATE: {
      const auto* d = static_cast<SQL_DATE_STRUCT*>(p.ptr);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "'%04d-%02u-%02u'", d->year, d->month, d->day);
      return buf;
    }
    case SQL_C_TYPE_TIME:
    case SQL_C_TIME: {
      const auto* t = static_cast<SQL_TIME_STRUCT*>(p.ptr);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "'%02u:%02u:%02u'", t->hour, t->minute, t->second);
      return buf;
    }
    case SQL_C_BINARY: {
      if (ind == SQL_NTS) {
        throw DiagError{"HY090", "Binary parameter requires an explicit length"};
      }
      const auto* data = static_cast<const unsigned char*>(p.ptr);
      return "'" + hexEncode(data, static_cast<size_t>(ind)) + "'";
    }
    default:
      throw DiagError{"07006",
                      "Unsupported C type for parameter " + std::to_string(oneBasedIndex)};
  }
}

}  // namespace

int countParameterMarkers(const std::string& sql) {
  int count = 0;
  scanParameterMarkers(sql, [&count](size_t) { count++; });
  return count;
}

std::string substituteParameters(PinotStmt* stmt, const std::string& sql) {
  std::vector<size_t> positions;
  scanParameterMarkers(sql, [&positions](size_t pos) { positions.push_back(pos); });
  if (positions.empty()) return sql;

  std::string out;
  out.reserve(sql.size() + positions.size() * 16);
  size_t prev = 0;
  for (size_t i = 0; i < positions.size(); i++) {
    auto it = stmt->params.find(static_cast<SQLUSMALLINT>(i + 1));
    if (it == stmt->params.end()) {
      throw DiagError{"07002", "No value bound for parameter " + std::to_string(i + 1)};
    }
    out.append(sql, prev, positions[i] - prev);
    out += renderParameter(it->second, static_cast<int>(i + 1));
    prev = positions[i] + 1;
  }
  out.append(sql, prev, std::string::npos);
  return out;
}

void executeStatement(PinotStmt* stmt, const std::string& sql) {
  if (!stmt->dbc->connected || stmt->dbc->client == nullptr) {
    throw DiagError{"08003", "Connection not open"};
  }
  std::string finalSql = substituteParameters(stmt, sql);

  stmt->rs.clear();
  stmt->hasResult = false;
  stmt->cursor = -1;
  stmt->getDataOffsets.clear();

  nlohmann::json resp =
      stmt->dbc->client->query(finalSql, static_cast<long>(stmt->queryTimeoutSec));
  ResultSet rs = resultSetFromQueryResponse(resp, stmt->dbc->cfg.stringColumnSize);
  if (stmt->maxRows > 0 && rs.rowCount() > stmt->maxRows) {
    // Honor SQL_ATTR_MAX_ROWS client-side.
    nlohmann::json trimmed = nlohmann::json::array();
    for (size_t i = 0; i < stmt->maxRows; i++) trimmed.push_back(rs.rows[i]);
    rs.rows = std::move(trimmed);
  }
  stmt->rs = std::move(rs);
  stmt->hasResult = true;
}

ResultSet makeCatalogResultSet(
    const std::vector<std::pair<std::string, PinotBaseType>>& columns, long stringColumnSize) {
  ResultSet rs;
  for (const auto& [name, base] : columns) {
    PinotType t;
    t.base = base;
    rs.columns.push_back(makeColumn(name, t, stringColumnSize));
  }
  return rs;
}

}  // namespace pinot_odbc
