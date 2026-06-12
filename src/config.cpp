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

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_ODBCINST
#include <odbcinst.h>
#endif

namespace pinot_odbc {

std::string toUpperAscii(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return r;
}

std::string trimAscii(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

bool parseBool(const std::string& v, bool dflt) {
  if (v.empty()) return dflt;
  std::string u = toUpperAscii(trimAscii(v));
  if (u == "1" || u == "TRUE" || u == "YES" || u == "Y" || u == "ON") return true;
  if (u == "0" || u == "FALSE" || u == "NO" || u == "N" || u == "OFF") return false;
  return dflt;
}

std::map<std::string, std::string> parseConnectionString(const std::string& in) {
  std::map<std::string, std::string> kv;
  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    while (i < n && (in[i] == ';' || std::isspace(static_cast<unsigned char>(in[i])))) i++;
    if (i >= n) break;
    size_t eq = in.find('=', i);
    if (eq == std::string::npos) break;
    std::string key = trimAscii(in.substr(i, eq - i));
    i = eq + 1;
    std::string val;
    if (i < n && in[i] == '{') {
      size_t close = in.find('}', i + 1);
      if (close == std::string::npos) {
        val = in.substr(i + 1);
        i = n;
      } else {
        val = in.substr(i + 1, close - i - 1);
        i = close + 1;
        while (i < n && in[i] != ';') i++;
      }
    } else {
      size_t sc = in.find(';', i);
      if (sc == std::string::npos) {
        val = trimAscii(in.substr(i));
        i = n;
      } else {
        val = trimAscii(in.substr(i, sc - i));
        i = sc;
      }
    }
    if (!key.empty()) kv[toUpperAscii(key)] = val;
  }
  return kv;
}

void mergeDsnProfile(std::map<std::string, std::string>& kv) {
#ifdef HAVE_ODBCINST
  auto it = kv.find("DSN");
  if (it == kv.end() || it->second.empty()) return;
  const std::string& dsn = it->second;
  static const char* kKeys[] = {
      "HOST",       "SERVER",       "PORT",         "SCHEME",       "BROKER",
      "CONTROLLER", "UID",          "USER",         "PWD",          "PASSWORD",
      "TOKEN",      "DATABASE",     "TIMEOUT",      "SSLVERIFY",    "USEMULTISTAGE",
      "QUERYOPTIONS", "STRINGCOLUMNSIZE", "HEALTHCHECK"};
  char buf[1024];
  for (const char* key : kKeys) {
    if (kv.count(key)) continue;
    buf[0] = '\0';
    int len = SQLGetPrivateProfileString(dsn.c_str(), key, "", buf, sizeof(buf), "odbc.ini");
    if (len > 0 && buf[0] != '\0') {
      kv[key] = std::string(buf, static_cast<size_t>(len));
    }
  }
#else
  (void)kv;
#endif
}

namespace {

// Splits "host:port" or a full URL into the config's broker/controller slots.
void applyEndpoint(const std::string& value, std::string* urlOverride, std::string* scheme,
                   std::string* host, int* port) {
  std::string v = trimAscii(value);
  if (v.empty()) return;
  if (v.find("://") != std::string::npos) {
    // Full URL; strip a trailing slash for clean path concatenation.
    while (!v.empty() && v.back() == '/') v.pop_back();
    *urlOverride = v;
    return;
  }
  size_t colon = v.rfind(':');
  if (colon != std::string::npos && colon + 1 < v.size() &&
      std::isdigit(static_cast<unsigned char>(v[colon + 1]))) {
    *host = v.substr(0, colon);
    *port = std::atoi(v.c_str() + colon + 1);
  } else {
    *host = v;
  }
  (void)scheme;
}

std::string getAny(const std::map<std::string, std::string>& kv,
                   std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    auto it = kv.find(k);
    if (it != kv.end() && !it->second.empty()) return it->second;
  }
  return "";
}

}  // namespace

Config configFromKeyValues(const std::map<std::string, std::string>& kv) {
  Config cfg;
  cfg.dsn = getAny(kv, {"DSN"});
  cfg.driver = getAny(kv, {"DRIVER"});

  std::string scheme = getAny(kv, {"SCHEME", "PROTOCOL"});
  if (!scheme.empty()) {
    std::string s = toUpperAscii(scheme);
    cfg.scheme = (s == "HTTPS") ? "https" : "http";
  }

  std::string host = getAny(kv, {"HOST", "SERVER", "BROKERHOST"});
  if (!host.empty()) cfg.host = host;
  std::string port = getAny(kv, {"PORT", "BROKERPORT"});
  if (!port.empty()) cfg.port = std::atoi(port.c_str());

  std::string broker = getAny(kv, {"BROKER"});
  if (!broker.empty()) {
    applyEndpoint(broker, &cfg.brokerUrlOverride, &cfg.scheme, &cfg.host, &cfg.port);
  }

  cfg.controllerScheme = cfg.scheme;
  cfg.controllerHost = cfg.host;
  std::string controller = getAny(kv, {"CONTROLLER", "CONTROLLERURL"});
  if (!controller.empty()) {
    applyEndpoint(controller, &cfg.controllerUrlOverride, &cfg.controllerScheme,
                  &cfg.controllerHost, &cfg.controllerPort);
  }
  std::string cport = getAny(kv, {"CONTROLLERPORT"});
  if (!cport.empty()) cfg.controllerPort = std::atoi(cport.c_str());
  std::string chost = getAny(kv, {"CONTROLLERHOST"});
  if (!chost.empty()) cfg.controllerHost = chost;

  cfg.uid = getAny(kv, {"UID", "USER", "USERNAME"});
  cfg.pwd = getAny(kv, {"PWD", "PASSWORD"});
  cfg.token = getAny(kv, {"TOKEN", "AUTHTOKEN"});
  cfg.database = getAny(kv, {"DATABASE", "DB"});
  cfg.queryOptions = getAny(kv, {"QUERYOPTIONS"});

  std::string timeout = getAny(kv, {"TIMEOUT", "QUERYTIMEOUT"});
  if (!timeout.empty()) {
    long t = std::atol(timeout.c_str());
    if (t > 0) cfg.timeoutSec = t;
  }
  std::string scs = getAny(kv, {"STRINGCOLUMNSIZE"});
  if (!scs.empty()) {
    long v = std::atol(scs.c_str());
    if (v > 0) cfg.stringColumnSize = v;
  }
  cfg.sslVerify = parseBool(getAny(kv, {"SSLVERIFY", "VERIFYSSL"}), true);
  cfg.useMultistage = parseBool(getAny(kv, {"USEMULTISTAGE", "MULTISTAGE"}), false);
  cfg.healthCheck = parseBool(getAny(kv, {"HEALTHCHECK"}), true);
  return cfg;
}

std::string Config::brokerBaseUrl() const {
  if (!brokerUrlOverride.empty()) return brokerUrlOverride;
  return scheme + "://" + host + ":" + std::to_string(port);
}

std::string Config::controllerBaseUrl() const {
  if (!controllerUrlOverride.empty()) return controllerUrlOverride;
  return controllerScheme + "://" + controllerHost + ":" + std::to_string(controllerPort);
}

std::string buildOutConnectionString(const Config& cfg) {
  std::string out;
  auto append = [&out](const std::string& key, const std::string& value) {
    if (value.empty()) return;
    if (!out.empty()) out += ";";
    if (value.find(';') != std::string::npos) {
      out += key + "={" + value + "}";
    } else {
      out += key + "=" + value;
    }
  };
  append("DSN", cfg.dsn);
  if (cfg.dsn.empty()) append("DRIVER", cfg.driver);
  if (!cfg.brokerUrlOverride.empty()) {
    append("BROKER", cfg.brokerUrlOverride);
  } else {
    append("HOST", cfg.host);
    append("PORT", std::to_string(cfg.port));
    append("SCHEME", cfg.scheme);
  }
  if (!cfg.controllerUrlOverride.empty()) {
    append("CONTROLLER", cfg.controllerUrlOverride);
  } else {
    append("CONTROLLER", cfg.controllerHost + ":" + std::to_string(cfg.controllerPort));
  }
  append("UID", cfg.uid);
  append("PWD", cfg.pwd);
  append("TOKEN", cfg.token);
  append("DATABASE", cfg.database);
  append("QUERYOPTIONS", cfg.queryOptions);
  if (cfg.useMultistage) append("USEMULTISTAGE", "1");
  if (!cfg.sslVerify) append("SSLVERIFY", "0");
  if (!cfg.healthCheck) append("HEALTHCHECK", "0");
  append("TIMEOUT", std::to_string(cfg.timeoutSec));
  return out;
}

}  // namespace pinot_odbc
