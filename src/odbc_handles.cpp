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

// Handle allocation/free, environment/connection/statement attributes,
// and diagnostics.

#include <sql.h>
#include <sqlext.h>

#include "api_helpers.h"
#include "handles.h"

using namespace pinot_odbc;

extern "C" {

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle,
                                 SQLHANDLE* OutputHandle) {
  if (OutputHandle == nullptr) return SQL_ERROR;
  *OutputHandle = SQL_NULL_HANDLE;
  switch (HandleType) {
    case SQL_HANDLE_ENV: {
      *OutputHandle = new PinotEnv();
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DBC: {
      PinotEnv* env = asEnv(InputHandle);
      if (env == nullptr) return SQL_INVALID_HANDLE;
      std::lock_guard<std::recursive_mutex> lock(env->mtx);
      env->diag.clear();
      auto* dbc = new PinotDbc(env);
      env->connections.insert(dbc);
      *OutputHandle = dbc;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_STMT: {
      PinotDbc* dbc = asDbc(InputHandle);
      if (dbc == nullptr) return SQL_INVALID_HANDLE;
      std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
      dbc->diag.clear();
      if (!dbc->connected) {
        dbc->diag.add("08003", "Connection not open");
        return SQL_ERROR;
      }
      auto* stmt = new PinotStmt(dbc);
      dbc->stmts.insert(stmt);
      *OutputHandle = stmt;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DESC: {
      PinotDbc* dbc = asDbc(InputHandle);
      if (dbc == nullptr) return SQL_INVALID_HANDLE;
      std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
      dbc->diag.clear();
      dbc->diag.add("HYC00", "Explicit descriptor handles are not supported");
      return SQL_ERROR;
    }
    default:
      return SQL_ERROR;
  }
}

static void destroyStmt(PinotStmt* stmt) {
  stmt->magic = kDeadMagic;
  delete stmt;
}

static void destroyDbc(PinotDbc* dbc) {
  for (PinotStmt* stmt : dbc->stmts) destroyStmt(stmt);
  dbc->stmts.clear();
  dbc->magic = kDeadMagic;
  delete dbc;
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle) {
  switch (HandleType) {
    case SQL_HANDLE_ENV: {
      PinotEnv* env = asEnv(Handle);
      if (env == nullptr) return SQL_INVALID_HANDLE;
      for (PinotDbc* dbc : env->connections) destroyDbc(dbc);
      env->connections.clear();
      env->magic = kDeadMagic;
      delete env;
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_DBC: {
      PinotDbc* dbc = asDbc(Handle);
      if (dbc == nullptr) return SQL_INVALID_HANDLE;
      if (dbc->env != nullptr) dbc->env->connections.erase(dbc);
      destroyDbc(dbc);
      return SQL_SUCCESS;
    }
    case SQL_HANDLE_STMT: {
      PinotStmt* stmt = asStmt(Handle);
      if (stmt == nullptr) return SQL_INVALID_HANDLE;
      if (stmt->dbc != nullptr) stmt->dbc->stmts.erase(stmt);
      destroyStmt(stmt);
      return SQL_SUCCESS;
    }
    default:
      return SQL_INVALID_HANDLE;
  }
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT StatementHandle, SQLUSMALLINT Option) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  switch (Option) {
    case SQL_CLOSE:
      stmt->rs.clear();
      stmt->hasResult = false;
      stmt->cursor = -1;
      stmt->getDataOffsets.clear();
      return SQL_SUCCESS;
    case SQL_UNBIND:
      stmt->bindings.clear();
      return SQL_SUCCESS;
    case SQL_RESET_PARAMS:
      stmt->params.clear();
      return SQL_SUCCESS;
    case SQL_DROP:
      if (stmt->dbc != nullptr) stmt->dbc->stmts.erase(stmt);
      destroyStmt(stmt);
      return SQL_SUCCESS;
    default:
      stmt->diag.add("HY092", "Invalid attribute/option identifier");
      return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT StatementHandle) {
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  if (!stmt->hasResult) {
    stmt->diag.add("24000", "Invalid cursor state");
    return SQL_ERROR;
  }
  stmt->rs.clear();
  stmt->hasResult = false;
  stmt->cursor = -1;
  stmt->getDataOffsets.clear();
  return SQL_SUCCESS;
}

// ---- environment attributes -------------------------------------------------

SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                SQLINTEGER StringLength) {
  (void)StringLength;
  PinotEnv* env = asEnv(EnvironmentHandle);
  if (env == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(env->mtx);
  env->diag.clear();
  switch (Attribute) {
    case SQL_ATTR_ODBC_VERSION:
      env->odbcVersion = static_cast<SQLINTEGER>(reinterpret_cast<SQLLEN>(Value));
      return SQL_SUCCESS;
    case SQL_ATTR_OUTPUT_NTS:
      if (reinterpret_cast<SQLLEN>(Value) != SQL_TRUE) {
        env->diag.add("HYC00", "SQL_ATTR_OUTPUT_NTS=SQL_FALSE is not supported");
        return SQL_ERROR;
      }
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_POOLING:
    case SQL_ATTR_CP_MATCH:
      return SQL_SUCCESS;  // pooling is handled by the driver manager
    default:
      env->diag.add("HY092", "Invalid attribute/option identifier");
      return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  (void)BufferLength;
  PinotEnv* env = asEnv(EnvironmentHandle);
  if (env == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(env->mtx);
  env->diag.clear();
  switch (Attribute) {
    case SQL_ATTR_ODBC_VERSION:
      if (Value != nullptr) *static_cast<SQLINTEGER*>(Value) = env->odbcVersion;
      if (StringLength != nullptr) *StringLength = sizeof(SQLINTEGER);
      return SQL_SUCCESS;
    case SQL_ATTR_OUTPUT_NTS:
      if (Value != nullptr) *static_cast<SQLINTEGER*>(Value) = SQL_TRUE;
      if (StringLength != nullptr) *StringLength = sizeof(SQLINTEGER);
      return SQL_SUCCESS;
    default:
      env->diag.add("HY092", "Invalid attribute/option identifier");
      return SQL_ERROR;
  }
}

// ---- connection attributes ----------------------------------------------------

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                    SQLPOINTER Value, SQLINTEGER StringLength) {
  (void)StringLength;
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  switch (Attribute) {
    case SQL_ATTR_AUTOCOMMIT:
      // Pinot has no transactions; accept either setting.
      dbc->autocommit = static_cast<SQLUINTEGER>(reinterpret_cast<SQLULEN>(Value));
      return SQL_SUCCESS;
    case SQL_ATTR_LOGIN_TIMEOUT:
    case SQL_ATTR_CONNECTION_TIMEOUT:
      dbc->loginTimeout = static_cast<SQLUINTEGER>(reinterpret_cast<SQLULEN>(Value));
      return SQL_SUCCESS;
    case SQL_ATTR_ACCESS_MODE:
    case SQL_ATTR_TXN_ISOLATION:
    case SQL_ATTR_CURRENT_CATALOG:
    case SQL_ATTR_METADATA_ID:
      return SQL_SUCCESS;  // accepted, no effect
    default:
      return SQL_SUCCESS;  // be permissive with unrecognized attributes
  }
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                    SQLPOINTER Value, SQLINTEGER BufferLength,
                                    SQLINTEGER* StringLength) {
  PinotDbc* dbc = asDbc(ConnectionHandle);
  if (dbc == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(dbc->mtx);
  dbc->diag.clear();
  switch (Attribute) {
    case SQL_ATTR_AUTOCOMMIT:
      if (Value != nullptr) *static_cast<SQLUINTEGER*>(Value) = dbc->autocommit;
      if (StringLength != nullptr) *StringLength = sizeof(SQLUINTEGER);
      return SQL_SUCCESS;
    case SQL_ATTR_LOGIN_TIMEOUT:
    case SQL_ATTR_CONNECTION_TIMEOUT:
      if (Value != nullptr) *static_cast<SQLUINTEGER*>(Value) = dbc->loginTimeout;
      if (StringLength != nullptr) *StringLength = sizeof(SQLUINTEGER);
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_DEAD:
      if (Value != nullptr) {
        *static_cast<SQLUINTEGER*>(Value) = dbc->connected ? SQL_CD_FALSE : SQL_CD_TRUE;
      }
      if (StringLength != nullptr) *StringLength = sizeof(SQLUINTEGER);
      return SQL_SUCCESS;
    case SQL_ATTR_ACCESS_MODE:
      if (Value != nullptr) *static_cast<SQLUINTEGER*>(Value) = SQL_MODE_READ_ONLY;
      if (StringLength != nullptr) *StringLength = sizeof(SQLUINTEGER);
      return SQL_SUCCESS;
    case SQL_ATTR_CURRENT_CATALOG:
      return copyStringToBuffer(dbc->diag, dbc->cfg.database,
                                static_cast<SQLCHAR*>(Value), BufferLength, StringLength);
    case SQL_ATTR_TXN_ISOLATION:
      if (Value != nullptr) *static_cast<SQLUINTEGER*>(Value) = 0;
      if (StringLength != nullptr) *StringLength = sizeof(SQLUINTEGER);
      return SQL_SUCCESS;
    default:
      dbc->diag.add("HY092", "Invalid attribute/option identifier");
      return SQL_ERROR;
  }
}

// ---- statement attributes -----------------------------------------------------

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                 SQLINTEGER StringLength) {
  (void)StringLength;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  switch (Attribute) {
    case SQL_ATTR_QUERY_TIMEOUT:
      stmt->queryTimeoutSec = reinterpret_cast<SQLULEN>(Value);
      return SQL_SUCCESS;
    case SQL_ATTR_MAX_ROWS:
      stmt->maxRows = reinterpret_cast<SQLULEN>(Value);
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_ARRAY_SIZE: {
      SQLULEN requested = reinterpret_cast<SQLULEN>(Value);
      stmt->rowArraySize = 1;
      if (requested != 1) {
        stmt->diag.add("01S02", "Option value changed: row array size forced to 1");
        return SQL_SUCCESS_WITH_INFO;
      }
      return SQL_SUCCESS;
    }
    case SQL_ATTR_ROWS_FETCHED_PTR:
      stmt->rowsFetchedPtr = static_cast<SQLULEN*>(Value);
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_STATUS_PTR:
      stmt->rowStatusPtr = static_cast<SQLUSMALLINT*>(Value);
      return SQL_SUCCESS;
    case SQL_ATTR_CURSOR_TYPE: {
      SQLULEN requested = reinterpret_cast<SQLULEN>(Value);
      if (requested != SQL_CURSOR_FORWARD_ONLY) {
        stmt->diag.add("01S02", "Option value changed: cursor type forced to forward-only");
        return SQL_SUCCESS_WITH_INFO;
      }
      return SQL_SUCCESS;
    }
    case SQL_ATTR_CONCURRENCY: {
      SQLULEN requested = reinterpret_cast<SQLULEN>(Value);
      if (requested != SQL_CONCUR_READ_ONLY) {
        stmt->diag.add("01S02", "Option value changed: concurrency forced to read-only");
        return SQL_SUCCESS_WITH_INFO;
      }
      return SQL_SUCCESS;
    }
    case SQL_ATTR_ROW_BIND_TYPE:
      if (reinterpret_cast<SQLULEN>(Value) != SQL_BIND_BY_COLUMN) {
        stmt->diag.add("HYC00", "Row-wise binding is not supported");
        return SQL_ERROR;
      }
      return SQL_SUCCESS;
    case SQL_ATTR_NOSCAN:
    case SQL_ATTR_RETRIEVE_DATA:
    case SQL_ATTR_USE_BOOKMARKS:
    case SQL_ATTR_PARAM_BIND_TYPE:
    case SQL_ATTR_PARAMSET_SIZE:
      return SQL_SUCCESS;  // accepted, no effect
    default:
      return SQL_SUCCESS;  // be permissive with unrecognized attributes
  }
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                 SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  (void)BufferLength;
  PinotStmt* stmt = asStmt(StatementHandle);
  if (stmt == nullptr) return SQL_INVALID_HANDLE;
  std::lock_guard<std::recursive_mutex> lock(stmt->mtx);
  stmt->diag.clear();
  auto setULen = [&](SQLULEN v) {
    if (Value != nullptr) *static_cast<SQLULEN*>(Value) = v;
    if (StringLength != nullptr) *StringLength = sizeof(SQLULEN);
  };
  switch (Attribute) {
    case SQL_ATTR_QUERY_TIMEOUT: setULen(stmt->queryTimeoutSec); return SQL_SUCCESS;
    case SQL_ATTR_MAX_ROWS: setULen(stmt->maxRows); return SQL_SUCCESS;
    case SQL_ATTR_ROW_ARRAY_SIZE: setULen(stmt->rowArraySize); return SQL_SUCCESS;
    case SQL_ATTR_CURSOR_TYPE: setULen(SQL_CURSOR_FORWARD_ONLY); return SQL_SUCCESS;
    case SQL_ATTR_CONCURRENCY: setULen(SQL_CONCUR_READ_ONLY); return SQL_SUCCESS;
    case SQL_ATTR_ROW_BIND_TYPE: setULen(SQL_BIND_BY_COLUMN); return SQL_SUCCESS;
    case SQL_ATTR_ROW_NUMBER:
      setULen(stmt->cursor >= 0 ? static_cast<SQLULEN>(stmt->cursor + 1) : 0);
      return SQL_SUCCESS;
    case SQL_ATTR_ROWS_FETCHED_PTR:
      if (Value != nullptr) *static_cast<SQLULEN**>(Value) = stmt->rowsFetchedPtr;
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_STATUS_PTR:
      if (Value != nullptr) *static_cast<SQLUSMALLINT**>(Value) = stmt->rowStatusPtr;
      return SQL_SUCCESS;
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC:
    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
      // Descriptor handles are not supported; return the statement handle so
      // that driver managers which blindly request them keep functioning.
      if (Value != nullptr) *static_cast<SQLHANDLE*>(Value) = StatementHandle;
      return SQL_SUCCESS;
    default:
      stmt->diag.add("HY092", "Invalid attribute/option identifier");
      return SQL_ERROR;
  }
}

// ---- diagnostics ---------------------------------------------------------------

static DiagList* diagFor(SQLSMALLINT type, SQLHANDLE handle) {
  switch (type) {
    case SQL_HANDLE_ENV: {
      PinotEnv* env = asEnv(handle);
      return env != nullptr ? &env->diag : nullptr;
    }
    case SQL_HANDLE_DBC: {
      PinotDbc* dbc = asDbc(handle);
      return dbc != nullptr ? &dbc->diag : nullptr;
    }
    case SQL_HANDLE_STMT: {
      PinotStmt* stmt = asStmt(handle);
      return stmt != nullptr ? &stmt->diag : nullptr;
    }
    default:
      return nullptr;
  }
}

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                SQLCHAR* SqlState, SQLINTEGER* NativeError, SQLCHAR* MessageText,
                                SQLSMALLINT BufferLength, SQLSMALLINT* TextLength) {
  DiagList* diag = diagFor(HandleType, Handle);
  if (diag == nullptr) return SQL_INVALID_HANDLE;
  if (RecNumber < 1) return SQL_ERROR;
  if (static_cast<size_t>(RecNumber) > diag->size()) return SQL_NO_DATA;
  const DiagRecord& rec = diag->at(static_cast<size_t>(RecNumber - 1));
  if (SqlState != nullptr) {
    std::string state = rec.sqlstate;
    state.resize(5, '0');
    std::memcpy(SqlState, state.data(), 5);
    SqlState[5] = '\0';
  }
  if (NativeError != nullptr) *NativeError = rec.native;
  std::string msg = "[PinotODBC] " + rec.message;
  if (TextLength != nullptr) *TextLength = static_cast<SQLSMALLINT>(msg.size());
  if (MessageText != nullptr && BufferLength > 0) {
    size_t ncopy = std::min(msg.size(), static_cast<size_t>(BufferLength - 1));
    std::memcpy(MessageText, msg.data(), ncopy);
    MessageText[ncopy] = '\0';
    if (ncopy < msg.size()) return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                  SQLSMALLINT DiagIdentifier, SQLPOINTER DiagInfo,
                                  SQLSMALLINT BufferLength, SQLSMALLINT* StringLength) {
  DiagList* diag = diagFor(HandleType, Handle);
  if (diag == nullptr) return SQL_INVALID_HANDLE;

  auto copyOut = [&](const std::string& s) -> SQLRETURN {
    if (StringLength != nullptr) *StringLength = static_cast<SQLSMALLINT>(s.size());
    if (DiagInfo != nullptr && BufferLength > 0) {
      size_t ncopy = std::min(s.size(), static_cast<size_t>(BufferLength - 1));
      std::memcpy(DiagInfo, s.data(), ncopy);
      static_cast<char*>(DiagInfo)[ncopy] = '\0';
      if (ncopy < s.size()) return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  };

  // Header fields.
  switch (DiagIdentifier) {
    case SQL_DIAG_NUMBER:
      if (DiagInfo != nullptr) *static_cast<SQLINTEGER*>(DiagInfo) =
          static_cast<SQLINTEGER>(diag->size());
      return SQL_SUCCESS;
    case SQL_DIAG_RETURNCODE:
      if (DiagInfo != nullptr) {
        *static_cast<SQLRETURN*>(DiagInfo) = diag->empty() ? SQL_SUCCESS : SQL_ERROR;
      }
      return SQL_SUCCESS;
    case SQL_DIAG_CURSOR_ROW_COUNT:
    case SQL_DIAG_ROW_COUNT: {
      PinotStmt* stmt = (HandleType == SQL_HANDLE_STMT) ? asStmt(Handle) : nullptr;
      if (DiagInfo != nullptr) {
        *static_cast<SQLLEN*>(DiagInfo) =
            (stmt != nullptr && stmt->hasResult) ? static_cast<SQLLEN>(stmt->rs.rowCount()) : 0;
      }
      return SQL_SUCCESS;
    }
    default:
      break;
  }

  // Record fields.
  if (RecNumber < 1 || static_cast<size_t>(RecNumber) > diag->size()) return SQL_NO_DATA;
  const DiagRecord& rec = diag->at(static_cast<size_t>(RecNumber - 1));
  switch (DiagIdentifier) {
    case SQL_DIAG_SQLSTATE: {
      std::string state = rec.sqlstate;
      state.resize(5, '0');
      return copyOut(state);
    }
    case SQL_DIAG_MESSAGE_TEXT:
      return copyOut("[PinotODBC] " + rec.message);
    case SQL_DIAG_NATIVE:
      if (DiagInfo != nullptr) *static_cast<SQLINTEGER*>(DiagInfo) = rec.native;
      return SQL_SUCCESS;
    case SQL_DIAG_CLASS_ORIGIN:
      return copyOut(rec.sqlstate.compare(0, 2, "IM") == 0 ? "ODBC 3.0" : "ISO 9075");
    case SQL_DIAG_SUBCLASS_ORIGIN:
      return copyOut("ISO 9075");
    case SQL_DIAG_CONNECTION_NAME:
    case SQL_DIAG_SERVER_NAME:
      return copyOut("");
    default:
      return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT CompletionType) {
  (void)CompletionType;
  if (HandleType == SQL_HANDLE_ENV) {
    return asEnv(Handle) != nullptr ? SQL_SUCCESS : SQL_INVALID_HANDLE;
  }
  if (HandleType == SQL_HANDLE_DBC) {
    // Pinot is read-only from ODBC's perspective; commit/rollback are no-ops.
    return asDbc(Handle) != nullptr ? SQL_SUCCESS : SQL_INVALID_HANDLE;
  }
  return SQL_INVALID_HANDLE;
}

SQLRETURN SQL_API SQLCancel(SQLHSTMT StatementHandle) {
  return asStmt(StatementHandle) != nullptr ? SQL_SUCCESS : SQL_INVALID_HANDLE;
}

}  // extern "C"
