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

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "handles.h"
#include "unicode_conv.h"

namespace pinot_odbc {

// ---- narrow/wide output buffer helpers ------------------------------------
//
// All helpers report the full (untruncated) length through lenPtr and add a
// 01004 diagnostic + SQL_SUCCESS_WITH_INFO when the buffer is too small,
// matching the ODBC contract for string output arguments.

template <typename LenT>
SQLRETURN copyStringToBuffer(DiagList& diag, const std::string& s, SQLCHAR* buf, SQLLEN bufLen,
                             LenT* lenPtr) {
  if (lenPtr != nullptr) *lenPtr = static_cast<LenT>(s.size());
  if (buf == nullptr) return SQL_SUCCESS;
  if (bufLen <= 0) {
    if (s.empty()) return SQL_SUCCESS;
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  size_t ncopy = std::min(s.size(), static_cast<size_t>(bufLen - 1));
  if (ncopy > 0) std::memcpy(buf, s.data(), ncopy);
  buf[ncopy] = '\0';
  if (ncopy < s.size()) {
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

// Wide variant with buffer length and reported length both in characters.
template <typename LenT>
SQLRETURN copyWideToBufferChars(DiagList& diag, const std::string& utf8, SQLWCHAR* buf,
                                SQLLEN bufLenChars, LenT* lenPtrChars) {
  WString w = utf8ToWide(utf8);
  if (lenPtrChars != nullptr) *lenPtrChars = static_cast<LenT>(w.size());
  if (buf == nullptr) return SQL_SUCCESS;
  if (bufLenChars <= 0) {
    if (w.empty()) return SQL_SUCCESS;
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  size_t ncopy = std::min(w.size(), static_cast<size_t>(bufLenChars - 1));
  if (ncopy > 0) std::memcpy(buf, w.data(), ncopy * sizeof(SQLWCHAR));
  buf[ncopy] = 0;
  if (ncopy < w.size()) {
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

// Wide variant with buffer length and reported length both in bytes
// (used by SQLGetInfoW and character SQLColAttributeW fields).
template <typename LenT>
SQLRETURN copyWideToBufferBytes(DiagList& diag, const std::string& utf8, SQLWCHAR* buf,
                                SQLLEN bufLenBytes, LenT* lenPtrBytes) {
  WString w = utf8ToWide(utf8);
  if (lenPtrBytes != nullptr) {
    *lenPtrBytes = static_cast<LenT>(w.size() * sizeof(SQLWCHAR));
  }
  if (buf == nullptr) return SQL_SUCCESS;
  SQLLEN bufLenChars = bufLenBytes / static_cast<SQLLEN>(sizeof(SQLWCHAR));
  if (bufLenChars <= 0) {
    if (w.empty()) return SQL_SUCCESS;
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  size_t ncopy = std::min(w.size(), static_cast<size_t>(bufLenChars - 1));
  if (ncopy > 0) std::memcpy(buf, w.data(), ncopy * sizeof(SQLWCHAR));
  buf[ncopy] = 0;
  if (ncopy < w.size()) {
    diag.add("01004", "String data, right truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

// ---- input string helpers ---------------------------------------------------

inline std::string fromSqlString(const SQLCHAR* s, SQLINTEGER len) {
  if (s == nullptr) return "";
  if (len == SQL_NTS) return std::string(reinterpret_cast<const char*>(s));
  if (len < 0) return "";
  return std::string(reinterpret_cast<const char*>(s), static_cast<size_t>(len));
}

// ---- misc utilities ---------------------------------------------------------

// SQL LIKE pattern match with % and _ wildcards and backslash escapes.
// An empty/missing pattern matches everything (lenient app compatibility).
bool likeMatch(const std::string& pattern, const std::string& value);

bool parseTimestampString(const std::string& s, SQL_TIMESTAMP_STRUCT& ts);
void timestampFromEpochMillis(long long ms, SQL_TIMESTAMP_STRUCT& ts);
std::string formatTimestamp(const SQL_TIMESTAMP_STRUCT& ts);

// Decodes a hex string ("4a6f65" or "4A6F65"); returns false on bad input.
bool hexDecode(const std::string& hex, std::vector<unsigned char>& out);
std::string hexEncode(const unsigned char* data, size_t len);

// Counts '?' parameter markers outside quotes/comments.
int countParameterMarkers(const std::string& sql);

// Replaces '?' markers with rendered literals from the statement's bound
// parameters. Throws DiagError (07002) when a marker has no binding.
std::string substituteParameters(PinotStmt* stmt, const std::string& sql);

// Executes SQL on the statement's connection and installs the result set.
// Returns SQL_SUCCESS or throws DiagError.
void executeStatement(PinotStmt* stmt, const std::string& sql);

// Builds an empty catalog result set with the given (name, type) columns.
ResultSet makeCatalogResultSet(
    const std::vector<std::pair<std::string, PinotBaseType>>& columns, long stringColumnSize);

}  // namespace pinot_odbc
