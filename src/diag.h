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
#include <vector>

#include <sql.h>
#include <sqlext.h>

namespace pinot_odbc {

// One ODBC diagnostic record (SQLSTATE + message + native error code).
struct DiagRecord {
  std::string sqlstate;  // always 5 characters
  std::string message;
  SQLINTEGER native = 0;
};

class DiagList {
 public:
  void clear() { records_.clear(); }

  void add(const std::string& sqlstate, const std::string& message, SQLINTEGER native = 0) {
    records_.push_back(DiagRecord{sqlstate, message, native});
  }

  bool empty() const { return records_.empty(); }
  size_t size() const { return records_.size(); }
  const DiagRecord& at(size_t i) const { return records_[i]; }

 private:
  std::vector<DiagRecord> records_;
};

// Internal exception that carries a diagnostic; caught at the API boundary
// and turned into a diag record + SQL_ERROR.
struct DiagError {
  std::string sqlstate;
  std::string message;
  SQLINTEGER native = 0;
};

}  // namespace pinot_odbc
