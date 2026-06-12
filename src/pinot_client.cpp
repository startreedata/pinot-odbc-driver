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

#include "pinot_client.h"

#include <curl/curl.h>

#include <cstring>
#include <mutex>

#include "diag.h"

namespace pinot_odbc {

namespace {

std::once_flag g_curlInitFlag;

void ensureCurlGlobalInit() {
  std::call_once(g_curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

}  // namespace

std::string base64Encode(const std::string& in) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                 (static_cast<unsigned char>(in[i + 1]) << 8) |
                 static_cast<unsigned char>(in[i + 2]);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                 (static_cast<unsigned char>(in[i + 1]) << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += "=";
  }
  return out;
}

PinotClient::PinotClient(const Config& cfg) : cfg_(cfg) {
  ensureCurlGlobalInit();
  curl_ = curl_easy_init();
  if (!curl_) {
    throw DiagError{"HY001", "Failed to initialize libcurl handle"};
  }
}

PinotClient::~PinotClient() {
  if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

PinotClient::HttpResponse PinotClient::request(const std::string& method, const std::string& url,
                                               const std::string& body, long timeoutSec,
                                               bool acceptJson) {
  std::lock_guard<std::mutex> lock(mtx_);
  CURL* curl = static_cast<CURL*>(curl_);
  curl_easy_reset(curl);

  HttpResponse resp;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec > 0 ? timeoutSec : cfg_.timeoutSec);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if (!cfg_.sslVerify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, acceptJson ? "Accept: application/json" : "Accept: */*");
  if (!cfg_.token.empty()) {
    // Allow either a bare token or a value that already includes the scheme
    // (e.g. "Basic dXNlcjpwYXNz").
    std::string value = cfg_.token.find(' ') != std::string::npos ? cfg_.token
                                                                  : "Bearer " + cfg_.token;
    headers = curl_slist_append(headers, ("Authorization: " + value).c_str());
  } else if (!cfg_.uid.empty()) {
    std::string credentials = cfg_.uid + ":" + cfg_.pwd;
    headers =
        curl_slist_append(headers, ("Authorization: Basic " + base64Encode(credentials)).c_str());
  }
  if (!cfg_.database.empty()) {
    headers = curl_slist_append(headers, ("database: " + cfg_.database).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    curl_slist_free_all(headers);
    std::string msg = std::string("HTTP request to ") + url + " failed: " + curl_easy_strerror(rc);
    if (rc == CURLE_OPERATION_TIMEDOUT) {
      throw DiagError{"HYT00", msg, static_cast<SQLINTEGER>(rc)};
    }
    if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
        rc == CURLE_COULDNT_RESOLVE_PROXY) {
      throw DiagError{"08001", msg, static_cast<SQLINTEGER>(rc)};
    }
    throw DiagError{"08S01", msg, static_cast<SQLINTEGER>(rc)};
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  curl_slist_free_all(headers);
  return resp;
}

nlohmann::json PinotClient::query(const std::string& sql, long timeoutSecOverride) {
  nlohmann::json req;
  req["sql"] = sql;
  std::string options = cfg_.queryOptions;
  if (cfg_.useMultistage) {
    if (!options.empty()) options += ";";
    options += "useMultistageEngine=true";
  }
  if (!options.empty()) req["queryOptions"] = options;

  std::string url = cfg_.brokerBaseUrl() + "/query/sql";
  HttpResponse resp = request("POST", url, req.dump(), timeoutSecOverride);

  if (resp.status == 401 || resp.status == 403) {
    throw DiagError{"28000", "Authentication failed against Pinot broker (HTTP " +
                                 std::to_string(resp.status) + "): " + resp.body,
                    static_cast<SQLINTEGER>(resp.status)};
  }
  nlohmann::json parsed = nlohmann::json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    throw DiagError{"08S01", "Invalid JSON response from Pinot broker (HTTP " +
                                 std::to_string(resp.status) +
                                 "): " + resp.body.substr(0, 512),
                    static_cast<SQLINTEGER>(resp.status)};
  }
  // Pinot reports query errors inside the JSON body, frequently with a
  // non-200 status as well; let the result builder surface those. A non-2xx
  // status without an exceptions array is a transport-level failure.
  bool hasExceptions = parsed.contains("exceptions") && parsed["exceptions"].is_array() &&
                       !parsed["exceptions"].empty();
  if ((resp.status < 200 || resp.status >= 300) && !hasExceptions) {
    throw DiagError{"08S01", "Pinot broker returned HTTP " + std::to_string(resp.status) + ": " +
                                 resp.body.substr(0, 512),
                    static_cast<SQLINTEGER>(resp.status)};
  }
  return parsed;
}

void PinotClient::checkHealth() {
  std::string url = cfg_.brokerBaseUrl() + "/health";
  // The health endpoint produces text/plain, so do not demand JSON.
  HttpResponse resp = request("GET", url, "", 0, /*acceptJson=*/false);
  if (resp.status < 200 || resp.status >= 300) {
    throw DiagError{"08001", "Pinot broker health check failed (HTTP " +
                                 std::to_string(resp.status) + ") at " + url,
                    static_cast<SQLINTEGER>(resp.status)};
  }
}

std::vector<std::string> PinotClient::listTables() {
  std::string url = cfg_.controllerBaseUrl() + "/tables";
  HttpResponse resp = request("GET", url, "", 0);
  if (resp.status == 401 || resp.status == 403) {
    throw DiagError{"28000", "Authentication failed against Pinot controller (HTTP " +
                                 std::to_string(resp.status) + ")",
                    static_cast<SQLINTEGER>(resp.status)};
  }
  if (resp.status < 200 || resp.status >= 300) {
    throw DiagError{"08S01", "Pinot controller returned HTTP " + std::to_string(resp.status) +
                                 " for " + url,
                    static_cast<SQLINTEGER>(resp.status)};
  }
  nlohmann::json parsed = nlohmann::json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded() || !parsed.contains("tables") || !parsed["tables"].is_array()) {
    throw DiagError{"08S01", "Unexpected response from Pinot controller /tables: " +
                                 resp.body.substr(0, 512)};
  }
  std::vector<std::string> tables;
  for (const auto& t : parsed["tables"]) {
    if (t.is_string()) tables.push_back(t.get<std::string>());
  }
  return tables;
}

nlohmann::json PinotClient::tableSchema(const std::string& tableName) {
  CURL* curl = static_cast<CURL*>(curl_);
  char* escaped = curl_easy_escape(curl, tableName.c_str(), static_cast<int>(tableName.size()));
  std::string encoded = escaped ? escaped : tableName;
  if (escaped) curl_free(escaped);

  std::string url = cfg_.controllerBaseUrl() + "/tables/" + encoded + "/schema";
  HttpResponse resp = request("GET", url, "", 0);
  if (resp.status < 200 || resp.status >= 300) {
    throw DiagError{"42S02", "Failed to fetch schema for table '" + tableName + "' (HTTP " +
                                 std::to_string(resp.status) + ")",
                    static_cast<SQLINTEGER>(resp.status)};
  }
  nlohmann::json parsed = nlohmann::json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    throw DiagError{"08S01", "Invalid JSON schema response for table '" + tableName + "'"};
  }
  return parsed;
}

}  // namespace pinot_odbc
