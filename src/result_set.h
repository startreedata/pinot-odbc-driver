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

#include "json.hpp"
#include "types.h"

namespace pinot_odbc {

struct Config;

struct Column {
  std::string name;
  PinotType pinotType;
  SqlTypeMeta meta;
};

Column makeColumn(const std::string& name, const PinotType& type, long stringColumnSize);

// An executed query's result: column metadata plus rows kept as parsed JSON
// (array of arrays). Cell values are converted lazily in SQLGetData/SQLFetch.
struct ResultSet {
  std::vector<Column> columns;
  nlohmann::json rows = nlohmann::json::array();

  size_t rowCount() const { return rows.is_array() ? rows.size() : 0; }
  void clear() {
    columns.clear();
    rows = nlohmann::json::array();
  }
};

// Builds a ResultSet from a Pinot broker query response. Throws DiagError if
// the response carries exceptions.
ResultSet resultSetFromQueryResponse(const nlohmann::json& resp, long stringColumnSize);

}  // namespace pinot_odbc
