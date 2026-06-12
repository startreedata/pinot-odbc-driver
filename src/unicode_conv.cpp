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

#include "unicode_conv.h"

#include <cstdint>

namespace pinot_odbc {

namespace {

constexpr uint32_t kReplacementChar = 0xFFFD;

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

std::string wideToUtf8(const SQLWCHAR* s, SQLINTEGER len) {
  std::string out;
  if (s == nullptr) return out;
  size_t n;
  if (len == SQL_NTS) {
    n = 0;
    while (s[n] != 0) n++;
  } else if (len < 0) {
    return out;
  } else {
    n = static_cast<size_t>(len);
  }
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = static_cast<uint32_t>(s[i]);
    if (sizeof(SQLWCHAR) == 2) {
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        // High surrogate: combine with the following low surrogate.
        if (i + 1 < n) {
          uint32_t low = static_cast<uint32_t>(s[i + 1]);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            i++;
          } else {
            cp = kReplacementChar;
          }
        } else {
          cp = kReplacementChar;
        }
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = kReplacementChar;
      }
    }
    if (cp > 0x10FFFF) cp = kReplacementChar;
    appendUtf8(out, cp);
  }
  return out;
}

WString utf8ToWide(const std::string& s) {
  WString out;
  out.reserve(s.size());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    uint32_t cp = kReplacementChar;
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t advance = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c1 & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (c1 & 0x3F);
        advance = 2;
      }
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x0F) << 12) |
             (static_cast<uint32_t>(c1 & 0x3F) << 6) | (c2 & 0x3F);
        advance = 3;
      }
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
      if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x07) << 18) |
             (static_cast<uint32_t>(c1 & 0x3F) << 12) |
             (static_cast<uint32_t>(c2 & 0x3F) << 6) | (c3 & 0x3F);
        advance = 4;
      }
    }
    i += advance;
    if (cp > 0x10FFFF) cp = kReplacementChar;
    if (sizeof(SQLWCHAR) == 2 && cp > 0xFFFF) {
      uint32_t v = cp - 0x10000;
      out += static_cast<SQLWCHAR>(0xD800 + (v >> 10));
      out += static_cast<SQLWCHAR>(0xDC00 + (v & 0x3FF));
    } else {
      out += static_cast<SQLWCHAR>(cp);
    }
  }
  return out;
}

}  // namespace pinot_odbc
