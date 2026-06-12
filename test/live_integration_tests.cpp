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

// Live integration tests against a real Apache Pinot cluster running the
// batch quickstart (baseballStats table). Typically launched via
// test/run_docker_integration.sh, which brings up the quickstart in Docker.
//
// Endpoints are taken from the environment:
//   PINOT_BROKER      host:port of the broker      (default localhost:8000)
//   PINOT_CONTROLLER  host:port of the controller  (default localhost:9000)

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include "test_util.h"
#include "unicode_conv.h"

using pinot_odbc::utf8ToWide;
using pinot_odbc::wideToUtf8;
using pinot_odbc::WString;

namespace {

std::string envOr(const char* name, const char* dflt) {
  const char* v = std::getenv(name);
  return (v != nullptr && *v != '\0') ? v : dflt;
}

std::string connString() {
  return "BROKER=" + envOr("PINOT_BROKER", "localhost:8000") +
         ";CONTROLLER=" + envOr("PINOT_CONTROLLER", "localhost:9000");
}

std::string diagText(SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLCHAR msg[2048] = {0};
  SQLSMALLINT len = 0;
  if (SQLGetDiagRec(type, h, 1, state, &native, msg, sizeof(msg), &len) == SQL_NO_DATA) {
    return "";
  }
  return std::string(reinterpret_cast<char*>(state)) + ": " +
         std::string(reinterpret_cast<char*>(msg));
}

SQLRETURN execSql(SQLHSTMT stmt, const std::string& sql) {
  SQLRETURN rc = SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
  if (!SQL_SUCCEEDED(rc)) {
    std::printf("query failed: %s\n  diag: %s\n", sql.c_str(),
                diagText(SQL_HANDLE_STMT, stmt).c_str());
  }
  return rc;
}

struct Session {
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  bool ok = false;

  Session() {
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) return;
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc))) return;
    std::string cs = connString();
    SQLCHAR out[2048];
    SQLSMALLINT outLen = 0;
    SQLRETURN rc = SQLDriverConnect(dbc, nullptr,
                                    reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
                                    SQL_NTS, out, sizeof(out), &outLen, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
      std::printf("connect failed (%s): %s\n", cs.c_str(),
                  diagText(SQL_HANDLE_DBC, dbc).c_str());
      return;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) return;
    ok = true;
  }

  ~Session() {
    if (stmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (ok) SQLDisconnect(dbc);
    if (dbc != SQL_NULL_HDBC) SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    if (env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
};

void testCatalog(Session& s) {
  // baseballStats must show up in SQLTables.
  CHECK(SQL_SUCCEEDED(SQLTables(s.stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0)));
  std::set<std::string> tables;
  char text[512];
  SQLLEN ind = 0;
  while (SQLFetch(s.stmt) == SQL_SUCCESS) {
    CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 3, SQL_C_CHAR, text, sizeof(text), &ind)));
    tables.insert(text);
  }
  std::printf("tables: ");
  for (const auto& t : tables) std::printf("%s ", t.c_str());
  std::printf("\n");
  CHECK(tables.count("baseballStats") == 1);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));

  // SQLColumns must report the well-known schema columns with correct types.
  CHECK(SQL_SUCCEEDED(
      SQLColumns(s.stmt, nullptr, 0, nullptr, 0,
                 reinterpret_cast<SQLCHAR*>(const_cast<char*>("baseballStats")), SQL_NTS,
                 nullptr, 0)));
  int colCount = 0;
  bool sawPlayerName = false, sawHomeRuns = false;
  while (SQLFetch(s.stmt) == SQL_SUCCESS) {
    colCount++;
    CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 4, SQL_C_CHAR, text, sizeof(text), &ind)));
    std::string name = text;
    SQLINTEGER sqlType = 0;
    CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 5, SQL_C_SLONG, &sqlType, sizeof(sqlType), &ind)));
    if (name == "playerName") {
      sawPlayerName = true;
      CHECK_EQ(sqlType, SQL_VARCHAR);
    }
    if (name == "homeRuns") {
      sawHomeRuns = true;
      CHECK_EQ(sqlType, SQL_INTEGER);
    }
  }
  std::printf("baseballStats has %d columns\n", colCount);
  CHECK(colCount > 10);
  CHECK(sawPlayerName);
  CHECK(sawHomeRuns);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
}

void testCount(Session& s) {
  CHECK(SQL_SUCCEEDED(execSql(s.stmt, "SELECT count(*) FROM baseballStats")));
  SQLSMALLINT numCols = 0;
  CHECK(SQL_SUCCEEDED(SQLNumResultCols(s.stmt, &numCols)));
  CHECK_EQ(numCols, 1);
  CHECK_EQ(SQLFetch(s.stmt), SQL_SUCCESS);
  long long count = 0;
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 1, SQL_C_SBIGINT, &count, sizeof(count), &ind)));
  std::printf("baseballStats count(*) = %lld\n", count);
  CHECK(count > 10000);  // dataset ships with ~97k rows
  CHECK_EQ(SQLFetch(s.stmt), SQL_NO_DATA);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
}

void testSelectRows(Session& s) {
  CHECK(SQL_SUCCEEDED(execSql(
      s.stmt,
      "SELECT playerName, yearID, homeRuns FROM baseballStats "
      "WHERE homeRuns > 0 ORDER BY homeRuns DESC, playerName LIMIT 10")));

  SQLSMALLINT numCols = 0;
  CHECK(SQL_SUCCEEDED(SQLNumResultCols(s.stmt, &numCols)));
  CHECK_EQ(numCols, 3);

  // Verify reported metadata matches the real schema types.
  SQLCHAR name[128];
  SQLSMALLINT nameLen = 0, dataType = 0, decimals = 0, nullable = 0;
  SQLULEN colSize = 0;
  CHECK(SQL_SUCCEEDED(SQLDescribeCol(s.stmt, 1, name, sizeof(name), &nameLen, &dataType,
                                     &colSize, &decimals, &nullable)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(name)), "playerName");
  CHECK_EQ(dataType, SQL_VARCHAR);
  CHECK(SQL_SUCCEEDED(SQLDescribeCol(s.stmt, 3, name, sizeof(name), &nameLen, &dataType,
                                     &colSize, &decimals, &nullable)));
  CHECK_EQ(dataType, SQL_INTEGER);

  // Bound columns; home runs must be positive and non-increasing.
  char player[256];
  SQLINTEGER year = 0, homeRuns = 0;
  SQLLEN indPlayer = 0, indYear = 0, indHr = 0;
  CHECK(SQL_SUCCEEDED(SQLBindCol(s.stmt, 1, SQL_C_CHAR, player, sizeof(player), &indPlayer)));
  CHECK(SQL_SUCCEEDED(SQLBindCol(s.stmt, 2, SQL_C_SLONG, &year, sizeof(year), &indYear)));
  CHECK(SQL_SUCCEEDED(SQLBindCol(s.stmt, 3, SQL_C_SLONG, &homeRuns, sizeof(homeRuns), &indHr)));

  int rows = 0;
  SQLINTEGER prevHr = 0;
  while (SQLFetch(s.stmt) == SQL_SUCCESS) {
    rows++;
    CHECK(indPlayer > 0);
    CHECK(homeRuns > 0);
    CHECK(year > 1800 && year < 2100);
    if (rows > 1) CHECK(homeRuns <= prevHr);
    prevHr = homeRuns;
    if (rows <= 3) std::printf("  %s (%d): %d HR\n", player, year, homeRuns);
  }
  CHECK_EQ(rows, 10);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_UNBIND)));
}

void testAggregationGroupBy(Session& s) {
  CHECK(SQL_SUCCEEDED(execSql(
      s.stmt,
      "SELECT playerName, sum(homeRuns) AS totalHR FROM baseballStats "
      "GROUP BY playerName ORDER BY totalHR DESC LIMIT 5")));
  int rows = 0;
  char player[256];
  SQLLEN ind = 0;
  double prev = 1e18;
  while (SQLFetch(s.stmt) == SQL_SUCCESS) {
    rows++;
    CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 1, SQL_C_CHAR, player, sizeof(player), &ind)));
    double total = 0;
    CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 2, SQL_C_DOUBLE, &total, sizeof(total), &ind)));
    CHECK(total > 100);  // all-time top-5 HR totals are in the hundreds
    CHECK(total <= prev);
    prev = total;
    std::printf("  top HR: %s = %.0f\n", player, total);
  }
  CHECK_EQ(rows, 5);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
}

void testParameterizedQuery(Session& s) {
  const char* sql =
      "SELECT count(*) FROM baseballStats WHERE yearID >= ? AND league = ?";
  CHECK(SQL_SUCCEEDED(
      SQLPrepare(s.stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS)));
  SQLSMALLINT numParams = 0;
  CHECK(SQL_SUCCEEDED(SQLNumParams(s.stmt, &numParams)));
  CHECK_EQ(numParams, 2);

  SQLINTEGER year = 2000;
  char league[] = "NL";
  SQLLEN leagueLen = SQL_NTS;
  CHECK(SQL_SUCCEEDED(SQLBindParameter(s.stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0,
                                       0, &year, 0, nullptr)));
  CHECK(SQL_SUCCEEDED(SQLBindParameter(s.stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0,
                                       league, sizeof(league), &leagueLen)));
  CHECK(SQL_SUCCEEDED(SQLExecute(s.stmt)));
  CHECK_EQ(SQLFetch(s.stmt), SQL_SUCCESS);
  long long count = 0;
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 1, SQL_C_SBIGINT, &count, sizeof(count), &ind)));
  std::printf("NL rows since 2000: %lld\n", count);
  CHECK(count > 0);

  // Re-execute with different parameter values (statement reuse).
  year = 1900;
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
  CHECK(SQL_SUCCEEDED(SQLExecute(s.stmt)));
  CHECK_EQ(SQLFetch(s.stmt), SQL_SUCCESS);
  long long count1900 = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 1, SQL_C_SBIGINT, &count1900, sizeof(count1900), &ind)));
  CHECK(count1900 >= count);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_RESET_PARAMS)));
}

void testUnicodePath(Session& s) {
  WString wsql = utf8ToWide("SELECT playerName FROM baseballStats LIMIT 1");
  CHECK(SQL_SUCCEEDED(SQLExecDirectW(s.stmt, const_cast<SQLWCHAR*>(wsql.c_str()),
                                     static_cast<SQLINTEGER>(wsql.size()))));
  CHECK_EQ(SQLFetch(s.stmt), SQL_SUCCESS);
  SQLWCHAR wtext[256];
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(s.stmt, 1, SQL_C_WCHAR, wtext, sizeof(wtext), &ind)));
  CHECK(!wideToUtf8(wtext, SQL_NTS).empty());
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.stmt, SQL_CLOSE)));
}

void testQueryErrorSurfaces(Session& s) {
  CHECK_EQ(SQLExecDirect(s.stmt,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                             "SELECT noSuchColumn FROM noSuchTableForOdbcTest")),
                         SQL_NTS),
           SQL_ERROR);
  std::string diag = diagText(SQL_HANDLE_STMT, s.stmt);
  std::printf("expected error diag: %.120s\n", diag.c_str());
  CHECK(!diag.empty());
}

}  // namespace

int main() {
  Session s;
  CHECK(s.ok);
  if (!s.ok) {
    std::printf(
        "Could not connect to Pinot at BROKER=%s CONTROLLER=%s.\n"
        "Start the quickstart first, e.g.: test/run_docker_integration.sh\n",
        envOr("PINOT_BROKER", "localhost:8000").c_str(),
        envOr("PINOT_CONTROLLER", "localhost:9000").c_str());
    return testExitCode("live_integration_tests");
  }
  testCatalog(s);
  testCount(s);
  testSelectRows(s);
  testAggregationGroupBy(s);
  testParameterizedQuery(s);
  testUnicodePath(s);
  testQueryErrorSurfaces(s);
  return testExitCode("live_integration_tests");
}
