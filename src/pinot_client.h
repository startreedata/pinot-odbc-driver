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

#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "json.hpp"

namespace pinot_odbc {

// HTTP client for the Pinot broker (queries) and controller (catalog
// metadata). Throws DiagError on transport and query errors.
class PinotClient {
 public:
  explicit PinotClient(const Config& cfg);
  ~PinotClient();

  PinotClient(const PinotClient&) = delete;
  PinotClient& operator=(const PinotClient&) = delete;

  // POST /query/sql on the broker. Returns the parsed JSON response with
  // exceptions already checked (throws DiagError if any are present at the
  // transport level; query-level exceptions are left to the result builder).
  nlohmann::json query(const std::string& sql, long timeoutSecOverride = 0);

  // GET /health on the broker; throws DiagError on failure.
  void checkHealth();

  // GET /tables on the controller -> table names.
  std::vector<std::string> listTables();

  // GET /tables/{name}/schema on the controller.
  nlohmann::json tableSchema(const std::string& tableName);

 private:
  struct HttpResponse {
    long status = 0;
    std::string body;
  };

  // acceptJson controls the Accept header: the broker /health endpoint
  // produces text/plain and rejects "Accept: application/json" with 406.
  HttpResponse request(const std::string& method, const std::string& url,
                       const std::string& body, long timeoutSec, bool acceptJson = true);

  Config cfg_;
  void* curl_ = nullptr;  // CURL*
  std::mutex mtx_;
};

std::string base64Encode(const std::string& in);

}  // namespace pinot_odbc
