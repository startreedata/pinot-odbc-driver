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
#include <sqlucode.h>

namespace pinot_odbc {

// Custom traits: libc++ deprecates char_traits for types other than the
// standard character types, and SQLWCHAR is unsigned short on unixODBC.
struct SqlWCharTraits {
  using char_type = SQLWCHAR;
  using int_type = unsigned int;
  using off_type = std::streamoff;
  using pos_type = std::streampos;
  using state_type = std::mbstate_t;

  static void assign(char_type& a, const char_type& b) noexcept { a = b; }
  static bool eq(char_type a, char_type b) noexcept { return a == b; }
  static bool lt(char_type a, char_type b) noexcept { return a < b; }
  static int compare(const char_type* a, const char_type* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
      if (a[i] < b[i]) return -1;
      if (a[i] > b[i]) return 1;
    }
    return 0;
  }
  static size_t length(const char_type* s) {
    size_t n = 0;
    while (s[n] != 0) n++;
    return n;
  }
  static const char_type* find(const char_type* s, size_t n, const char_type& c) {
    for (size_t i = 0; i < n; i++) {
      if (s[i] == c) return s + i;
    }
    return nullptr;
  }
  static char_type* move(char_type* d, const char_type* s, size_t n) {
    return static_cast<char_type*>(memmove(d, s, n * sizeof(char_type)));
  }
  static char_type* copy(char_type* d, const char_type* s, size_t n) {
    return static_cast<char_type*>(memcpy(d, s, n * sizeof(char_type)));
  }
  static char_type* assign(char_type* d, size_t n, char_type c) {
    for (size_t i = 0; i < n; i++) d[i] = c;
    return d;
  }
  static int_type not_eof(int_type i) noexcept { return i == eof() ? 0 : i; }
  static char_type to_char_type(int_type i) noexcept { return static_cast<char_type>(i); }
  static int_type to_int_type(char_type c) noexcept { return static_cast<int_type>(c); }
  static bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
  static int_type eof() noexcept { return static_cast<int_type>(-1); }
};

using WString = std::basic_string<SQLWCHAR, SqlWCharTraits>;

// Converts a driver-manager wide string (UTF-16 when sizeof(SQLWCHAR)==2,
// UTF-32 otherwise) to UTF-8. len is in SQLWCHAR units, or SQL_NTS.
std::string wideToUtf8(const SQLWCHAR* s, SQLINTEGER len);

WString utf8ToWide(const std::string& s);

}  // namespace pinot_odbc
