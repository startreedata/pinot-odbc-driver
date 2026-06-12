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

// End-to-end ODBC API tests against an in-process mock Pinot broker.
// The driver objects are linked directly, so SQL* calls resolve to this
// driver without going through a driver manager.

#include <cstring>
#include <string>

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include "mock_broker.h"
#include "test_util.h"
#include "unicode_conv.h"

using pinot_odbc::utf8ToWide;
using pinot_odbc::wideToUtf8;
using pinot_odbc::WString;

namespace {

std::string connString(const MockBroker& broker, const std::string& extra = "") {
  // The mock serves both broker and controller endpoints on one port.
  std::string s = "HOST=127.0.0.1;PORT=" + std::to_string(broker.port()) +
                  ";SCHEME=http;CONTROLLER=127.0.0.1:" + std::to_string(broker.port());
  if (!extra.empty()) s += ";" + extra;
  return s;
}

std::string diagText(SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLCHAR msg[1024] = {0};
  SQLSMALLINT len = 0;
  if (SQLGetDiagRec(type, h, 1, state, &native, msg, sizeof(msg), &len) == SQL_NO_DATA) {
    return "";
  }
  return std::string(reinterpret_cast<char*>(state)) + ": " +
         std::string(reinterpret_cast<char*>(msg));
}

struct OdbcSession {
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  bool connected = false;

  SQLRETURN connect(const std::string& cs) {
    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    if (!SQL_SUCCEEDED(rc)) return rc;
    rc = SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    if (!SQL_SUCCEEDED(rc)) return rc;
    rc = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
    if (!SQL_SUCCEEDED(rc)) return rc;
    SQLCHAR out[2048];
    SQLSMALLINT outLen = 0;
    rc = SQLDriverConnect(dbc, nullptr,
                          reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())), SQL_NTS, out,
                          sizeof(out), &outLen, SQL_DRIVER_NOPROMPT);
    connected = SQL_SUCCEEDED(rc);
    return rc;
  }

  ~OdbcSession() {
    if (connected) SQLDisconnect(dbc);
    if (dbc != SQL_NULL_HDBC) SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    if (env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
};

void testConnectAndGetInfo(MockBroker& broker) {
  OdbcSession s;
  SQLRETURN rc = s.connect(connString(broker));
  CHECK(SQL_SUCCEEDED(rc));
  if (!SQL_SUCCEEDED(rc)) {
    std::printf("connect failed: %s\n", diagText(SQL_HANDLE_DBC, s.dbc).c_str());
    return;
  }

  SQLCHAR buf[256];
  SQLSMALLINT len = 0;
  CHECK(SQL_SUCCEEDED(SQLGetInfo(s.dbc, SQL_DBMS_NAME, buf, sizeof(buf), &len)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), "Apache Pinot");
  CHECK(SQL_SUCCEEDED(SQLGetInfo(s.dbc, SQL_DRIVER_ODBC_VER, buf, sizeof(buf), &len)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), "03.51");

  SQLUSMALLINT txn = 99;
  CHECK(SQL_SUCCEEDED(SQLGetInfo(s.dbc, SQL_TXN_CAPABLE, &txn, sizeof(txn), nullptr)));
  CHECK_EQ(txn, SQL_TC_NONE);

  SQLUINTEGER getdata = 0;
  CHECK(SQL_SUCCEEDED(SQLGetInfo(s.dbc, SQL_GETDATA_EXTENSIONS, &getdata, sizeof(getdata),
                                 nullptr)));
  CHECK((getdata & SQL_GD_ANY_COLUMN) != 0);

  // SQLGetFunctions: bitmap and individual.
  SQLUSMALLINT supported = SQL_FALSE;
  CHECK(SQL_SUCCEEDED(SQLGetFunctions(s.dbc, SQL_API_SQLEXECDIRECT, &supported)));
  CHECK_EQ(supported, SQL_TRUE);
  SQLUSMALLINT bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE];
  CHECK(SQL_SUCCEEDED(SQLGetFunctions(s.dbc, SQL_API_ODBC3_ALL_FUNCTIONS, bitmap)));
  CHECK(SQL_FUNC_EXISTS(bitmap, SQL_API_SQLFETCH) == SQL_TRUE);
  CHECK(SQL_FUNC_EXISTS(bitmap, SQL_API_SQLTABLES) == SQL_TRUE);

  // Autocommit toggling must be accepted (pyodbc defaults to OFF).
  CHECK(SQL_SUCCEEDED(SQLSetConnectAttr(s.dbc, SQL_ATTR_AUTOCOMMIT,
                                        reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0)));
  CHECK(SQL_SUCCEEDED(SQLEndTran(SQL_HANDLE_DBC, s.dbc, SQL_COMMIT)));
}

void testSelectAndConversions(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));

  SQLRETURN rc = SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM testTable")), SQL_NTS);
  CHECK(SQL_SUCCEEDED(rc));
  if (!SQL_SUCCEEDED(rc)) {
    std::printf("exec failed: %s\n", diagText(SQL_HANDLE_STMT, stmt).c_str());
    return;
  }

  SQLSMALLINT numCols = 0;
  CHECK(SQL_SUCCEEDED(SQLNumResultCols(stmt, &numCols)));
  CHECK_EQ(numCols, 11);

  SQLLEN rowCount = -1;
  CHECK(SQL_SUCCEEDED(SQLRowCount(stmt, &rowCount)));
  CHECK_EQ(rowCount, 3);

  // Column metadata.
  SQLCHAR name[128];
  SQLSMALLINT nameLen = 0, dataType = 0, decimals = 0, nullable = 0;
  SQLULEN colSize = 0;
  CHECK(SQL_SUCCEEDED(
      SQLDescribeCol(stmt, 1, name, sizeof(name), &nameLen, &dataType, &colSize, &decimals,
                     &nullable)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(name)), "intCol");
  CHECK_EQ(dataType, SQL_INTEGER);
  CHECK(SQL_SUCCEEDED(
      SQLDescribeCol(stmt, 2, name, sizeof(name), &nameLen, &dataType, &colSize, &decimals,
                     &nullable)));
  CHECK_EQ(dataType, SQL_BIGINT);
  CHECK(SQL_SUCCEEDED(
      SQLDescribeCol(stmt, 7, name, sizeof(name), &nameLen, &dataType, &colSize, &decimals,
                     &nullable)));
  CHECK_EQ(dataType, SQL_TYPE_TIMESTAMP);
  CHECK(SQL_SUCCEEDED(
      SQLDescribeCol(stmt, 8, name, sizeof(name), &nameLen, &dataType, &colSize, &decimals,
                     &nullable)));
  CHECK_EQ(dataType, SQL_VARBINARY);

  // SQLColAttribute.
  SQLLEN numAttr = 0;
  CHECK(SQL_SUCCEEDED(SQLColAttribute(stmt, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr,
                                      &numAttr)));
  CHECK_EQ(numAttr, SQL_INTEGER);
  SQLCHAR typeName[64];
  SQLSMALLINT typeNameLen = 0;
  CHECK(SQL_SUCCEEDED(SQLColAttribute(stmt, 2, SQL_DESC_TYPE_NAME, typeName, sizeof(typeName),
                                      &typeNameLen, nullptr)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(typeName)), "LONG");

  // ---- row 1: typed SQLGetData ----
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);

  SQLINTEGER i32 = 0;
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_SLONG, &i32, sizeof(i32), &ind)));
  CHECK_EQ(i32, 123);

  long long i64 = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 2, SQL_C_SBIGINT, &i64, sizeof(i64), &ind)));
  CHECK_EQ(i64, 1234567890123LL);

  double dbl = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 3, SQL_C_DOUBLE, &dbl, sizeof(dbl), &ind)));
  CHECK_EQ(dbl, 1.5);
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 4, SQL_C_DOUBLE, &dbl, sizeof(dbl), &ind)));
  CHECK_EQ(dbl, 2.25);

  char text[256];
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 5, SQL_C_CHAR, text, sizeof(text), &ind)));
  CHECK_EQ(std::string(text), "hello world from Pinot");
  CHECK_EQ(ind, static_cast<SQLLEN>(std::strlen("hello world from Pinot")));

  unsigned char bit = 0xFF;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 6, SQL_C_BIT, &bit, sizeof(bit), &ind)));
  CHECK_EQ(static_cast<int>(bit), 1);

  SQL_TIMESTAMP_STRUCT ts;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 7, SQL_C_TYPE_TIMESTAMP, &ts, sizeof(ts), &ind)));
  CHECK_EQ(ts.year, 2024);
  CHECK_EQ(ts.month, 3);
  CHECK_EQ(ts.day, 15);
  CHECK_EQ(ts.hour, 10);
  CHECK_EQ(ts.fraction, 456000000u);

  unsigned char bin[16];
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 8, SQL_C_BINARY, bin, sizeof(bin), &ind)));
  CHECK_EQ(ind, 5);  // hex "48656c6c6f" -> "Hello"
  CHECK(std::memcmp(bin, "Hello", 5) == 0);

  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 9, SQL_C_CHAR, text, sizeof(text), &ind)));
  CHECK_EQ(std::string(text), "{\"a\":1}");

  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 10, SQL_C_CHAR, text, sizeof(text), &ind)));
  CHECK_EQ(std::string(text), "12345.6789");

  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 11, SQL_C_CHAR, text, sizeof(text), &ind)));
  CHECK_EQ(std::string(text), "[1,2,3]");  // MV column as JSON text

  // ---- row 2: chunked SQLGetData ----
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  char tiny[2];  // 1 data byte + NUL per call
  std::string assembled;
  SQLRETURN grc;
  // Column 7 (timestamp) rendered as char: "2020-01-01 00:00:00.0".
  while ((grc = SQLGetData(stmt, 7, SQL_C_CHAR, tiny, sizeof(tiny), &ind)) != SQL_NO_DATA) {
    CHECK(SQL_SUCCEEDED(grc));
    assembled += tiny;
    if (grc == SQL_SUCCESS) break;  // final chunk
  }
  CHECK_EQ(assembled, "2020-01-01 00:00:00.0");

  SQLINTEGER negative = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_SLONG, &negative, sizeof(negative), &ind)));
  CHECK_EQ(negative, -7);

  // Numeric overflow must error with 22003.
  unsigned char tinyInt = 0;
  CHECK_EQ(SQLGetData(stmt, 2, SQL_C_UTINYINT, &tinyInt, sizeof(tinyInt), &ind), SQL_ERROR);
  CHECK(diagText(SQL_HANDLE_STMT, stmt).find("22003") == 0);

  // ---- row 3: NULLs ----
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_SLONG, &i32, sizeof(i32), &ind)));
  CHECK_EQ(ind, SQL_NULL_DATA);
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 5, SQL_C_CHAR, text, sizeof(text), &ind)));
  CHECK_EQ(ind, SQL_NULL_DATA);

  // ---- end of data ----
  CHECK_EQ(SQLFetch(stmt), SQL_NO_DATA);
  CHECK_EQ(SQLMoreResults(stmt), SQL_NO_DATA);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testBoundColumns(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));

  SQLINTEGER i32 = 0;
  char str[64];
  double dbl = 0;
  SQLLEN ind1 = 0, ind5 = 0, ind4 = 0;
  CHECK(SQL_SUCCEEDED(SQLBindCol(stmt, 1, SQL_C_SLONG, &i32, sizeof(i32), &ind1)));
  CHECK(SQL_SUCCEEDED(SQLBindCol(stmt, 5, SQL_C_CHAR, str, sizeof(str), &ind5)));
  CHECK(SQL_SUCCEEDED(SQLBindCol(stmt, 4, SQL_C_DOUBLE, &dbl, sizeof(dbl), &ind4)));

  SQLULEN rowsFetched = 0;
  SQLUSMALLINT rowStatus[1] = {0};
  CHECK(SQL_SUCCEEDED(SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &rowsFetched, 0)));
  CHECK(SQL_SUCCEEDED(SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, rowStatus, 0)));

  CHECK(SQL_SUCCEEDED(SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM testTable")), SQL_NTS)));

  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  CHECK_EQ(i32, 123);
  CHECK_EQ(std::string(str), "hello world from Pinot");
  CHECK_EQ(dbl, 2.25);
  CHECK_EQ(rowsFetched, 1u);
  CHECK_EQ(rowStatus[0], SQL_ROW_SUCCESS);

  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  CHECK_EQ(i32, -7);
  CHECK_EQ(std::string(str), "x");

  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);  // NULL row
  CHECK_EQ(ind1, SQL_NULL_DATA);
  CHECK_EQ(ind5, SQL_NULL_DATA);

  CHECK_EQ(SQLFetch(stmt), SQL_NO_DATA);
  CHECK_EQ(rowsFetched, 0u);

  // Re-execute after SQLFreeStmt(SQL_CLOSE) with unbind.
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_UNBIND)));
  CHECK(SQL_SUCCEEDED(SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM testTable")), SQL_NTS)));
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testQueryError(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));

  SQLRETURN rc = SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT err")), SQL_NTS);
  CHECK_EQ(rc, SQL_ERROR);

  SQLCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLCHAR msg[512] = {0};
  SQLSMALLINT len = 0;
  CHECK(SQL_SUCCEEDED(
      SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, msg, sizeof(msg), &len)));
  CHECK_EQ(std::string(reinterpret_cast<char*>(state)), "HY000");
  CHECK_EQ(native, 700);
  CHECK(std::string(reinterpret_cast<char*>(msg)).find("QueryValidationError") !=
        std::string::npos);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testCatalog(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));

  // SQLTables: all tables.
  CHECK(SQL_SUCCEEDED(SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0)));
  SQLSMALLINT numCols = 0;
  CHECK(SQL_SUCCEEDED(SQLNumResultCols(stmt, &numCols)));
  CHECK_EQ(numCols, 5);
  int tableCount = 0;
  std::string firstTable;
  char text[256];
  SQLLEN ind = 0;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &ind)));
    if (tableCount == 0) firstTable = text;
    tableCount++;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 4, SQL_C_CHAR, text, sizeof(text), &ind)));
    CHECK_EQ(std::string(text), "TABLE");
    // TABLE_CAT is NULL.
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &ind)));
    CHECK_EQ(ind, SQL_NULL_DATA);
  }
  CHECK_EQ(tableCount, 2);
  CHECK_EQ(firstTable, "airlineStats");
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));

  // SQLTables with pattern.
  CHECK(SQL_SUCCEEDED(SQLTables(stmt, nullptr, 0, nullptr, 0,
                                reinterpret_cast<SQLCHAR*>(const_cast<char*>("test%")), SQL_NTS,
                                nullptr, 0)));
  tableCount = 0;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &ind)));
    CHECK_EQ(std::string(text), "testTable");
    tableCount++;
  }
  CHECK_EQ(tableCount, 1);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));

  // SQLColumns for testTable.
  CHECK(SQL_SUCCEEDED(SQLColumns(stmt, nullptr, 0, nullptr, 0,
                                 reinterpret_cast<SQLCHAR*>(const_cast<char*>("testTable")),
                                 SQL_NTS, nullptr, 0)));
  CHECK(SQL_SUCCEEDED(SQLNumResultCols(stmt, &numCols)));
  CHECK_EQ(numCols, 18);
  int colCount = 0;
  bool sawIntCol = false, sawArrCol = false;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    colCount++;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 4, SQL_C_CHAR, text, sizeof(text), &ind)));
    std::string colName = text;
    SQLINTEGER sqlType = 0;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 5, SQL_C_SLONG, &sqlType, sizeof(sqlType), &ind)));
    if (colName == "intCol") {
      sawIntCol = true;
      CHECK_EQ(sqlType, SQL_INTEGER);
    }
    if (colName == "arrCol") {
      sawArrCol = true;
      CHECK_EQ(sqlType, SQL_VARCHAR);  // MV column
      CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 6, SQL_C_CHAR, text, sizeof(text), &ind)));
      CHECK_EQ(std::string(text), "INT_ARRAY");
    }
  }
  CHECK_EQ(colCount, 5);  // intCol, strCol, arrCol, doubleCol, tsCol
  CHECK(sawIntCol);
  CHECK(sawArrCol);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));

  // SQLPrimaryKeys.
  CHECK(SQL_SUCCEEDED(SQLPrimaryKeys(stmt, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<SQLCHAR*>(const_cast<char*>("testTable")),
                                     SQL_NTS)));
  int pkCount = 0;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    pkCount++;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 4, SQL_C_CHAR, text, sizeof(text), &ind)));
    CHECK_EQ(std::string(text), "intCol");
    SQLINTEGER seq = 0;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 5, SQL_C_SLONG, &seq, sizeof(seq), &ind)));
    CHECK_EQ(seq, 1);
  }
  CHECK_EQ(pkCount, 1);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));

  // SQLGetTypeInfo: contains the STRING type mapped to VARCHAR.
  CHECK(SQL_SUCCEEDED(SQLGetTypeInfo(stmt, SQL_ALL_TYPES)));
  bool sawString = false;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &ind)));
    SQLINTEGER dt = 0;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 2, SQL_C_SLONG, &dt, sizeof(dt), &ind)));
    if (std::string(text) == "STRING") {
      sawString = true;
      CHECK_EQ(dt, SQL_VARCHAR);
    }
  }
  CHECK(sawString);
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));

  // SQLGetTypeInfo filtered to SQL_BIGINT only.
  CHECK(SQL_SUCCEEDED(SQLGetTypeInfo(stmt, SQL_BIGINT)));
  int typeRows = 0;
  while (SQLFetch(stmt) == SQL_SUCCESS) {
    typeRows++;
    CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &ind)));
    CHECK_EQ(std::string(text), "LONG");
  }
  CHECK_EQ(typeRows, 1);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testPreparedStatementWithParams(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));

  const char* sql = "SELECT * FROM t WHERE name = ? AND age > ?";
  CHECK(SQL_SUCCEEDED(
      SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS)));

  SQLSMALLINT numParams = 0;
  CHECK(SQL_SUCCEEDED(SQLNumParams(stmt, &numParams)));
  CHECK_EQ(numParams, 2);

  char nameVal[] = "O'Brien";
  SQLLEN nameLen = SQL_NTS;
  SQLINTEGER ageVal = 30;
  CHECK(SQL_SUCCEEDED(SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                                       nameVal, sizeof(nameVal), &nameLen)));
  CHECK(SQL_SUCCEEDED(SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0,
                                       &ageVal, 0, nullptr)));
  CHECK(SQL_SUCCEEDED(SQLExecute(stmt)));

  // The mock echoes unknown SQL back as a single-row result.
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  char echoed[512];
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, echoed, sizeof(echoed), &ind)));
  CHECK_EQ(std::string(echoed), "SELECT * FROM t WHERE name = 'O''Brien' AND age > 30");

  // NULL parameter.
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));
  SQLLEN nullInd = SQL_NULL_DATA;
  CHECK(SQL_SUCCEEDED(SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                                       nameVal, sizeof(nameVal), &nullInd)));
  CHECK(SQL_SUCCEEDED(SQLExecute(stmt)));
  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, echoed, sizeof(echoed), &ind)));
  CHECK_EQ(std::string(echoed), "SELECT * FROM t WHERE name = NULL AND age > 30");

  // Missing binding -> 07002.
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_CLOSE)));
  CHECK(SQL_SUCCEEDED(SQLFreeStmt(stmt, SQL_RESET_PARAMS)));
  CHECK_EQ(SQLExecute(stmt), SQL_ERROR);
  CHECK(diagText(SQL_HANDLE_STMT, stmt).find("07002") == 0);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testUnicodeApi(MockBroker& broker) {
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)));
  CHECK(SQL_SUCCEEDED(
      SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0)));
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc)));

  WString wcs = utf8ToWide(connString(broker));
  SQLWCHAR out[1024];
  SQLSMALLINT outLen = 0;
  SQLRETURN rc = SQLDriverConnectW(dbc, nullptr, const_cast<SQLWCHAR*>(wcs.c_str()),
                                   static_cast<SQLSMALLINT>(wcs.size()), out, 1024, &outLen,
                                   SQL_DRIVER_NOPROMPT);
  CHECK(SQL_SUCCEEDED(rc));
  CHECK(wideToUtf8(out, outLen).find("HOST=127.0.0.1") != std::string::npos);

  // SQLGetInfoW returns wide strings with byte lengths.
  SQLWCHAR infoBuf[128];
  SQLSMALLINT infoLen = 0;
  CHECK(SQL_SUCCEEDED(SQLGetInfoW(dbc, SQL_DBMS_NAME, infoBuf, sizeof(infoBuf), &infoLen)));
  CHECK_EQ(wideToUtf8(infoBuf, SQL_NTS), "Apache Pinot");
  CHECK_EQ(infoLen, static_cast<SQLSMALLINT>(std::strlen("Apache Pinot") * sizeof(SQLWCHAR)));

  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt)));
  WString wsql = utf8ToWide("SELECT * FROM testTable");
  CHECK(SQL_SUCCEEDED(SQLExecDirectW(stmt, const_cast<SQLWCHAR*>(wsql.c_str()),
                                     static_cast<SQLINTEGER>(wsql.size()))));

  SQLWCHAR wname[128];
  SQLSMALLINT wnameLen = 0, dataType = 0, decimals = 0, nullable = 0;
  SQLULEN colSize = 0;
  CHECK(SQL_SUCCEEDED(SQLDescribeColW(stmt, 5, wname, 128, &wnameLen, &dataType, &colSize,
                                      &decimals, &nullable)));
  CHECK_EQ(wideToUtf8(wname, SQL_NTS), "strCol");
  CHECK_EQ(dataType, SQL_VARCHAR);

  CHECK_EQ(SQLFetch(stmt), SQL_SUCCESS);
  SQLWCHAR wtext[128];
  SQLLEN ind = 0;
  CHECK(SQL_SUCCEEDED(SQLGetData(stmt, 5, SQL_C_WCHAR, wtext, sizeof(wtext), &ind)));
  CHECK_EQ(wideToUtf8(wtext, SQL_NTS), "hello world from Pinot");
  CHECK_EQ(ind, static_cast<SQLLEN>(std::strlen("hello world from Pinot") * sizeof(SQLWCHAR)));

  // Error path through the W diagnostics API.
  WString werr = utf8ToWide("SELECT err");
  CHECK_EQ(SQLExecDirectW(stmt, const_cast<SQLWCHAR*>(werr.c_str()),
                          static_cast<SQLINTEGER>(werr.size())),
           SQL_ERROR);
  SQLWCHAR wstate[6];
  SQLWCHAR wmsg[512];
  SQLINTEGER native = 0;
  SQLSMALLINT msgLen = 0;
  CHECK(SQL_SUCCEEDED(
      SQLGetDiagRecW(SQL_HANDLE_STMT, stmt, 1, wstate, &native, wmsg, 512, &msgLen)));
  CHECK_EQ(wideToUtf8(wstate, 5), "HY000");
  CHECK_EQ(native, 700);

  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  SQLDisconnect(dbc);
  SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);
}

void testQueryOptionsAndMultistage(MockBroker& broker) {
  OdbcSession s;
  CHECK(SQL_SUCCEEDED(s.connect(connString(broker, "USEMULTISTAGE=1;QUERYOPTIONS=timeoutMs=5000"))));
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));
  CHECK(SQL_SUCCEEDED(SQLExecDirect(
      stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT options")), SQL_NTS)));
  CHECK_EQ(broker.lastQueryOptions(), "timeoutMs=5000;useMultistageEngine=true");
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

void testAuth() {
  MockBroker authBroker(/*requireAuth=*/true);

  // Wrong/missing token -> connection fails (health check gets 401).
  {
    OdbcSession s;
    SQLRETURN rc = s.connect(connString(authBroker));
    CHECK_EQ(rc, SQL_ERROR);
  }
  // Correct bearer token -> connect and query succeed.
  {
    OdbcSession s;
    CHECK(SQL_SUCCEEDED(s.connect(connString(authBroker, "TOKEN=secret"))));
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, s.dbc, &stmt)));
    CHECK(SQL_SUCCEEDED(SQLExecDirect(
        stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM testTable")),
        SQL_NTS)));
    SQLSMALLINT numCols = 0;
    CHECK(SQL_SUCCEEDED(SQLNumResultCols(stmt, &numCols)));
    CHECK_EQ(numCols, 11);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  }
}

void testConnectionRefused() {
  OdbcSession s;
  // Nothing listens on this port (reserved/unlikely).
  SQLRETURN rc = s.connect("HOST=127.0.0.1;PORT=1;TIMEOUT=2");
  CHECK_EQ(rc, SQL_ERROR);
  // POSIX loopback refuses immediately (08001/08S01); Windows may silently
  // drop instead, surfacing as a connect timeout (HYT00). Both are valid.
  std::string diag = diagText(SQL_HANDLE_DBC, s.dbc);
  CHECK(diag.find("08") == 0 || diag.find("HYT") == 0);
}

}  // namespace

int main() {
  MockBroker broker;
  testConnectAndGetInfo(broker);
  testSelectAndConversions(broker);
  testBoundColumns(broker);
  testQueryError(broker);
  testCatalog(broker);
  testPreparedStatementWithParams(broker);
  testUnicodeApi(broker);
  testQueryOptionsAndMultistage(broker);
  testAuth();
  testConnectionRefused();
  return testExitCode("integration_tests");
}
