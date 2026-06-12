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

#include <cstring>

#include "api_helpers.h"
#include "config.h"
#include "pinot_client.h"
#include "test_util.h"
#include "types.h"
#include "unicode_conv.h"

using namespace pinot_odbc;

static void testConnectionStringParsing() {
  auto kv = parseConnectionString("HOST=broker.example.com;PORT=8099;UID=u;PWD={p;w=d};foo=");
  CHECK_EQ(kv["HOST"], "broker.example.com");
  CHECK_EQ(kv["PORT"], "8099");
  CHECK_EQ(kv["UID"], "u");
  CHECK_EQ(kv["PWD"], "p;w=d");
  CHECK(kv.count("FOO") == 1);

  // Keys are case-insensitive, whitespace is trimmed.
  kv = parseConnectionString("  host = h1 ; Port=1234;");
  CHECK_EQ(kv["HOST"], "h1");
  CHECK_EQ(kv["PORT"], "1234");
}

static void testConfigDefaultsAndAliases() {
  Config cfg = configFromKeyValues(parseConnectionString("HOST=h;PORT=1234"));
  CHECK_EQ(cfg.host, "h");
  CHECK_EQ(cfg.port, 1234);
  CHECK_EQ(cfg.scheme, "http");
  CHECK_EQ(cfg.controllerHost, "h");
  CHECK_EQ(cfg.controllerPort, 9000);
  CHECK_EQ(cfg.brokerBaseUrl(), "http://h:1234");
  CHECK_EQ(cfg.controllerBaseUrl(), "http://h:9000");
  CHECK(cfg.sslVerify);
  CHECK(!cfg.useMultistage);

  cfg = configFromKeyValues(parseConnectionString(
      "BROKER=b:8000;CONTROLLER=https://ctrl.example.com:9443;SCHEME=https;"
      "USEMULTISTAGE=true;SSLVERIFY=0;TIMEOUT=5;TOKEN=tok;DATABASE=mydb"));
  CHECK_EQ(cfg.host, "b");
  CHECK_EQ(cfg.port, 8000);
  CHECK_EQ(cfg.scheme, "https");
  CHECK_EQ(cfg.brokerBaseUrl(), "https://b:8000");
  CHECK_EQ(cfg.controllerBaseUrl(), "https://ctrl.example.com:9443");
  CHECK(cfg.useMultistage);
  CHECK(!cfg.sslVerify);
  CHECK_EQ(cfg.timeoutSec, 5L);
  CHECK_EQ(cfg.token, "tok");
  CHECK_EQ(cfg.database, "mydb");

  // SERVER/USER/PASSWORD aliases.
  cfg = configFromKeyValues(parseConnectionString("SERVER=s;USER=alice;PASSWORD=pw"));
  CHECK_EQ(cfg.host, "s");
  CHECK_EQ(cfg.uid, "alice");
  CHECK_EQ(cfg.pwd, "pw");
}

static void testLikeMatch() {
  CHECK(likeMatch("", "anything"));
  CHECK(likeMatch("%", "anything"));
  CHECK(likeMatch("test%", "testTable"));
  CHECK(!likeMatch("test%", "myTestTable"));
  CHECK(likeMatch("%Table", "testTable"));
  CHECK(likeMatch("te_tTable", "testTable"));
  CHECK(!likeMatch("te_Table", "testTable"));
  CHECK(likeMatch("exact", "exact"));
  CHECK(!likeMatch("exact", "exact2"));
  CHECK(likeMatch("a\\%b", "a%b"));
  CHECK(!likeMatch("a\\%b", "aXb"));
  CHECK(likeMatch("%dim%", "myDimdimTable"));
}

static void testTypeMapping() {
  PinotType t = pinotTypeFromString("INT");
  CHECK(t.base == PinotBaseType::Int && !t.isArray);
  t = pinotTypeFromString("LONG_ARRAY");
  CHECK(t.base == PinotBaseType::Long && t.isArray);
  CHECK_EQ(pinotTypeName(t), "LONG_ARRAY");
  t = pinotTypeFromString("BIG_DECIMAL");
  CHECK(t.base == PinotBaseType::BigDecimal);

  SqlTypeMeta m = sqlTypeMetaFor(pinotTypeFromString("LONG"), 4096);
  CHECK_EQ(m.sqlType, SQL_BIGINT);
  CHECK(m.isSigned);
  m = sqlTypeMetaFor(pinotTypeFromString("TIMESTAMP"), 4096);
  CHECK_EQ(m.sqlType, SQL_TYPE_TIMESTAMP);
  CHECK_EQ(m.sqlDataType, SQL_DATETIME);
  CHECK_EQ(m.datetimeSub, SQL_CODE_TIMESTAMP);
  m = sqlTypeMetaFor(pinotTypeFromString("STRING"), 2222);
  CHECK_EQ(m.sqlType, SQL_VARCHAR);
  CHECK_EQ(static_cast<long>(m.columnSize), 2222L);
  m = sqlTypeMetaFor(pinotTypeFromString("STRING_ARRAY"), 4096);
  CHECK_EQ(m.sqlType, SQL_VARCHAR);  // arrays surface as JSON text
  m = sqlTypeMetaFor(pinotTypeFromString("BYTES"), 4096);
  CHECK_EQ(m.sqlType, SQL_VARBINARY);
}

static void testHex() {
  std::vector<unsigned char> bytes;
  CHECK(hexDecode("48656c6C6f", bytes));
  CHECK_EQ(std::string(bytes.begin(), bytes.end()), "Hello");
  CHECK_EQ(hexEncode(bytes.data(), bytes.size()), "48656c6c6f");
  CHECK(!hexDecode("abc", bytes));   // odd length
  CHECK(!hexDecode("zz", bytes));    // invalid digits
  CHECK(hexDecode("", bytes));
  CHECK(bytes.empty());
}

static void testTimestamps() {
  SQL_TIMESTAMP_STRUCT ts;
  CHECK(parseTimestampString("2024-03-15 10:20:30.456", ts));
  CHECK_EQ(ts.year, 2024);
  CHECK_EQ(ts.month, 3);
  CHECK_EQ(ts.day, 15);
  CHECK_EQ(ts.hour, 10);
  CHECK_EQ(ts.minute, 20);
  CHECK_EQ(ts.second, 30);
  CHECK_EQ(ts.fraction, 456000000u);  // nanoseconds
  CHECK_EQ(formatTimestamp(ts), "2024-03-15 10:20:30.456");

  CHECK(parseTimestampString("2020-01-01", ts));
  CHECK_EQ(ts.year, 2020);
  CHECK_EQ(ts.hour, 0);
  CHECK_EQ(formatTimestamp(ts), "2020-01-01 00:00:00");

  CHECK(parseTimestampString("2021-07-04T08:09:10", ts));
  CHECK_EQ(ts.hour, 8);

  CHECK(!parseTimestampString("not a timestamp", ts));
  CHECK(!parseTimestampString("2021-13-01", ts));

  timestampFromEpochMillis(1700000000123LL, ts);  // 2023-11-14 22:13:20.123 UTC
  CHECK_EQ(ts.year, 2023);
  CHECK_EQ(ts.month, 11);
  CHECK_EQ(ts.day, 14);
  CHECK_EQ(ts.fraction, 123000000u);
}

static void testUnicodeConversion() {
  // Round trip: ASCII, BMP, and astral plane (requires surrogate pairs when
  // SQLWCHAR is 16-bit).
  const std::string original = u8"héllo 中文 😀";
  WString w = utf8ToWide(original);
  std::string back = wideToUtf8(w.data(), static_cast<SQLINTEGER>(w.size()));
  CHECK_EQ(back, original);

  // SQL_NTS termination.
  WString z = utf8ToWide("abc");
  z += static_cast<SQLWCHAR>(0);
  CHECK_EQ(wideToUtf8(z.data(), SQL_NTS), "abc");

  if (sizeof(SQLWCHAR) == 2) {
    WString emoji = utf8ToWide(u8"😀");
    CHECK_EQ(emoji.size(), 2u);  // surrogate pair
  }
}

static void testParameterMarkers() {
  CHECK_EQ(countParameterMarkers("SELECT * FROM t WHERE a = ? AND b = ?"), 2);
  CHECK_EQ(countParameterMarkers("SELECT '?' FROM t WHERE a = ?"), 1);
  CHECK_EQ(countParameterMarkers("SELECT \"?col\" FROM t"), 0);
  CHECK_EQ(countParameterMarkers("SELECT 1 -- is this a ?\nFROM t"), 0);
  CHECK_EQ(countParameterMarkers("SELECT /* ? */ ? FROM t"), 1);
  CHECK_EQ(countParameterMarkers("SELECT 'O''Brien?' FROM t"), 0);
}

static void testBase64() {
  CHECK_EQ(base64Encode("user:pass"), "dXNlcjpwYXNz");
  CHECK_EQ(base64Encode(""), "");
  CHECK_EQ(base64Encode("a"), "YQ==");
  CHECK_EQ(base64Encode("ab"), "YWI=");
  CHECK_EQ(base64Encode("abc"), "YWJj");
}

static void testOutConnectionString() {
  Config cfg = configFromKeyValues(
      parseConnectionString("HOST=h;PORT=8099;UID=u;PWD=p;USEMULTISTAGE=1"));
  std::string out = buildOutConnectionString(cfg);
  CHECK(out.find("HOST=h") != std::string::npos);
  CHECK(out.find("PORT=8099") != std::string::npos);
  CHECK(out.find("UID=u") != std::string::npos);
  CHECK(out.find("USEMULTISTAGE=1") != std::string::npos);
}

int main() {
  testConnectionStringParsing();
  testConfigDefaultsAndAliases();
  testLikeMatch();
  testTypeMapping();
  testHex();
  testTimestamps();
  testUnicodeConversion();
  testParameterMarkers();
  testBase64();
  testOutConnectionString();
  return testExitCode("unit_tests");
}
