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

#include "result_set.h"

#include "diag.h"

namespace pinot_odbc {

Column makeColumn(const std::string& name, const PinotType& type, long stringColumnSize) {
  Column c;
  c.name = name;
  c.pinotType = type;
  c.meta = sqlTypeMetaFor(type, stringColumnSize);
  return c;
}

ResultSet resultSetFromQueryResponse(const nlohmann::json& resp, long stringColumnSize) {
  if (resp.contains("exceptions") && resp["exceptions"].is_array() &&
      !resp["exceptions"].empty()) {
    const auto& ex = resp["exceptions"][0];
    SQLINTEGER code = 0;
    std::string message = "Pinot query error";
    if (ex.is_object()) {
      if (ex.contains("errorCode") && ex["errorCode"].is_number()) {
        code = ex["errorCode"].get<SQLINTEGER>();
      }
      if (ex.contains("message") && ex["message"].is_string()) {
        message = ex["message"].get<std::string>();
      }
    }
    throw DiagError{"HY000", message, code};
  }

  ResultSet rs;
  if (!resp.contains("resultTable") || !resp["resultTable"].is_object()) {
    return rs;  // metadata-only response; empty result
  }
  const auto& table = resp["resultTable"];
  if (table.contains("dataSchema") && table["dataSchema"].is_object()) {
    const auto& schema = table["dataSchema"];
    const auto& names = schema.value("columnNames", nlohmann::json::array());
    const auto& types = schema.value("columnDataTypes", nlohmann::json::array());
    for (size_t i = 0; i < names.size(); i++) {
      std::string name = names[i].is_string() ? names[i].get<std::string>()
                                              : ("col" + std::to_string(i + 1));
      std::string typeStr =
          (i < types.size() && types[i].is_string()) ? types[i].get<std::string>() : "STRING";
      rs.columns.push_back(makeColumn(name, pinotTypeFromString(typeStr), stringColumnSize));
    }
  }
  if (table.contains("rows") && table["rows"].is_array()) {
    rs.rows = table["rows"];
  }
  return rs;
}

}  // namespace pinot_odbc
