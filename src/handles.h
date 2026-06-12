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

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <sql.h>
#include <sqlext.h>

#include "config.h"
#include "diag.h"
#include "pinot_client.h"
#include "result_set.h"

namespace pinot_odbc {

constexpr uint32_t kEnvMagic = 0x50454E56;   // "PENV"
constexpr uint32_t kDbcMagic = 0x50444243;   // "PDBC"
constexpr uint32_t kStmtMagic = 0x50535454;  // "PSTT"
constexpr uint32_t kDeadMagic = 0xDEADDEAD;

struct PinotDbc;
struct PinotStmt;

struct PinotEnv {
  uint32_t magic = kEnvMagic;
  std::recursive_mutex mtx;
  DiagList diag;
  SQLINTEGER odbcVersion = SQL_OV_ODBC3;
  std::set<PinotDbc*> connections;
};

struct PinotDbc {
  uint32_t magic = kDbcMagic;
  PinotEnv* env = nullptr;
  std::recursive_mutex mtx;
  DiagList diag;

  Config cfg;
  bool connected = false;
  std::unique_ptr<PinotClient> client;
  std::set<PinotStmt*> stmts;

  SQLUINTEGER autocommit = SQL_AUTOCOMMIT_ON;  // accepted and ignored
  SQLUINTEGER loginTimeout = 0;

  explicit PinotDbc(PinotEnv* e) : env(e) {}
};

struct ColumnBinding {
  SQLSMALLINT cType = SQL_C_DEFAULT;
  SQLPOINTER ptr = nullptr;
  SQLLEN bufLen = 0;
  SQLLEN* indPtr = nullptr;
};

struct ParamBinding {
  SQLSMALLINT ioType = SQL_PARAM_INPUT;
  SQLSMALLINT cType = SQL_C_DEFAULT;
  SQLSMALLINT sqlType = SQL_VARCHAR;
  SQLULEN columnSize = 0;
  SQLSMALLINT decimalDigits = 0;
  SQLPOINTER ptr = nullptr;
  SQLLEN bufLen = 0;
  SQLLEN* indPtr = nullptr;
};

struct PinotStmt {
  uint32_t magic = kStmtMagic;
  PinotDbc* dbc = nullptr;
  std::recursive_mutex mtx;
  DiagList diag;

  std::string sql;       // text from SQLPrepare
  bool prepared = false;

  bool hasResult = false;
  ResultSet rs;
  // Cursor position: -1 before the first SQLFetch, otherwise a row index.
  long cursor = -1;

  std::map<SQLUSMALLINT, ColumnBinding> bindings;
  std::map<SQLUSMALLINT, ParamBinding> params;
  // Per-column byte offset for chunked SQLGetData; -1 means fully consumed.
  std::map<SQLUSMALLINT, SQLLEN> getDataOffsets;

  // Statement attributes (only the commonly used subset).
  SQLULEN rowArraySize = 1;
  SQLULEN* rowsFetchedPtr = nullptr;
  SQLUSMALLINT* rowStatusPtr = nullptr;
  SQLULEN queryTimeoutSec = 0;
  SQLULEN maxRows = 0;
  std::string cursorName = "SQL_CUR_PINOT";

  explicit PinotStmt(PinotDbc* d) : dbc(d) {}
};

inline PinotEnv* asEnv(SQLHANDLE h) {
  auto* e = reinterpret_cast<PinotEnv*>(h);
  return (e != nullptr && e->magic == kEnvMagic) ? e : nullptr;
}

inline PinotDbc* asDbc(SQLHANDLE h) {
  auto* d = reinterpret_cast<PinotDbc*>(h);
  return (d != nullptr && d->magic == kDbcMagic) ? d : nullptr;
}

inline PinotStmt* asStmt(SQLHANDLE h) {
  auto* s = reinterpret_cast<PinotStmt*>(h);
  return (s != nullptr && s->magic == kStmtMagic) ? s : nullptr;
}

}  // namespace pinot_odbc
