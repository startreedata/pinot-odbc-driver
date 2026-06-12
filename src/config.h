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

#include <map>
#include <string>

namespace pinot_odbc {

// Connection configuration parsed from an ODBC connection string and/or a
// DSN entry in odbc.ini. Connection string keys (case-insensitive):
//
//   DSN, DRIVER         standard ODBC keys
//   HOST / SERVER       Pinot broker host (default localhost)
//   PORT                Pinot broker port (default 8099)
//   SCHEME              http or https (default http)
//   BROKER              broker as host:port or full URL (overrides HOST/PORT)
//   CONTROLLER          controller as host:port or full URL
//                       (default: broker host, port 9000; used for catalog
//                       metadata: SQLTables / SQLColumns / SQLPrimaryKeys)
//   UID / USER, PWD / PASSWORD   basic auth credentials
//   TOKEN               Authorization header value; "Bearer " is prepended
//                       unless the value already contains a scheme prefix
//   DATABASE            Pinot database name (sent as the "database" header)
//   TIMEOUT             HTTP timeout in seconds (default 60)
//   SSLVERIFY           0/1 verify TLS certificates (default 1)
//   USEMULTISTAGE       0/1 use the multi-stage query engine (default 0)
//   QUERYOPTIONS        raw Pinot queryOptions string, e.g. "timeoutMs=5000"
//   STRINGCOLUMNSIZE    reported column size for STRING columns (default 4096)
//   HEALTHCHECK         0/1 probe broker /health on connect (default 1)
struct Config {
  std::string dsn;
  std::string driver;

  std::string scheme = "http";
  std::string host = "localhost";
  int port = 8099;
  std::string brokerUrlOverride;  // full URL if BROKER was given as a URL

  std::string controllerScheme;  // defaults to scheme
  std::string controllerHost;    // defaults to host
  int controllerPort = 9000;
  std::string controllerUrlOverride;

  std::string uid;
  std::string pwd;
  std::string token;
  std::string database;
  std::string queryOptions;
  long timeoutSec = 60;
  bool sslVerify = true;
  bool useMultistage = false;
  long stringColumnSize = 4096;
  bool healthCheck = true;

  std::string brokerBaseUrl() const;
  std::string controllerBaseUrl() const;
};

// Parses "KEY=value;KEY2={value;with;semicolons}" into an uppercase-keyed map.
std::map<std::string, std::string> parseConnectionString(const std::string& s);

// Fills missing keys from the DSN's odbc.ini section (no-op without odbcinst).
void mergeDsnProfile(std::map<std::string, std::string>& kv);

// Builds a Config from parsed key/values, applying aliases and defaults.
Config configFromKeyValues(const std::map<std::string, std::string>& kv);

// Canonical out-connection-string echoed back from SQLDriverConnect.
std::string buildOutConnectionString(const Config& cfg);

bool parseBool(const std::string& v, bool dflt);
std::string toUpperAscii(const std::string& s);
std::string trimAscii(const std::string& s);

}  // namespace pinot_odbc
