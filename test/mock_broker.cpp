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

#include "mock_broker.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "json.hpp"

using nlohmann::json;

namespace {

constexpr MockSocket kInvalidSocket = static_cast<MockSocket>(-1);

#ifdef _WIN32
void ensureSocketsInitialized() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
  });
}
void closeSocket(MockSocket fd) { closesocket(static_cast<SOCKET>(fd)); }
void shutdownSocket(MockSocket fd) { shutdown(static_cast<SOCKET>(fd), SD_BOTH); }
int pollReadable(MockSocket fd, int timeoutMs) {
  WSAPOLLFD pfd{static_cast<SOCKET>(fd), POLLIN, 0};
  return WSAPoll(&pfd, 1, timeoutMs);
}
int recvSome(MockSocket fd, char* buf, int len) {
  return ::recv(static_cast<SOCKET>(fd), buf, len, 0);
}
void sendAll(MockSocket fd, const char* data, size_t len) {
  ::send(static_cast<SOCKET>(fd), data, static_cast<int>(len), 0);
}
#else
void ensureSocketsInitialized() {}
void closeSocket(MockSocket fd) { ::close(fd); }
void shutdownSocket(MockSocket fd) { ::shutdown(fd, SHUT_RDWR); }
int pollReadable(MockSocket fd, int timeoutMs) {
  pollfd pfd{fd, POLLIN, 0};
  return ::poll(&pfd, 1, timeoutMs);
}
int recvSome(MockSocket fd, char* buf, int len) {
  return static_cast<int>(::recv(fd, buf, static_cast<size_t>(len), 0));
}
void sendAll(MockSocket fd, const char* data, size_t len) {
  ::send(fd, data, len, 0);
}
#endif

json fullResultResponse() {
  json resp;
  resp["resultTable"]["dataSchema"]["columnNames"] = {
      "intCol", "longCol", "floatCol", "doubleCol", "strCol",  "boolCol",
      "tsCol",  "bytesCol", "jsonCol", "decCol",    "arrCol"};
  resp["resultTable"]["dataSchema"]["columnDataTypes"] = {
      "INT",       "LONG",  "FLOAT", "DOUBLE",      "STRING", "BOOLEAN",
      "TIMESTAMP", "BYTES", "JSON",  "BIG_DECIMAL", "INT_ARRAY"};
  resp["resultTable"]["rows"] = json::array({
      json::array({123, 1234567890123LL, 1.5, 2.25, "hello world from Pinot", true,
                   "2024-03-15 10:20:30.456", "48656c6c6f", "{\"a\":1}", "12345.6789",
                   json::array({1, 2, 3})}),
      json::array({-7, -42, -0.5, 3.14159, "x", false, "2020-01-01 00:00:00.0", "00ff",
                   "[1,2]", "-1.5", json::array({9})}),
      json::array({nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr}),
  });
  resp["exceptions"] = json::array();
  resp["numDocsScanned"] = 3;
  resp["totalDocs"] = 3;
  return resp;
}

json echoResponse(const std::string& value, const std::string& columnName) {
  json resp;
  resp["resultTable"]["dataSchema"]["columnNames"] = {columnName};
  resp["resultTable"]["dataSchema"]["columnDataTypes"] = {"STRING"};
  resp["resultTable"]["rows"] = json::array({json::array({value})});
  resp["exceptions"] = json::array();
  return resp;
}

json errorResponse() {
  json resp;
  resp["exceptions"] = json::array(
      {{{"errorCode", 700}, {"message", "QueryValidationError: bad query from mock"}}});
  return resp;
}

json testTableSchema() {
  json schema;
  schema["schemaName"] = "testTable";
  schema["dimensionFieldSpecs"] = json::array({
      {{"name", "intCol"}, {"dataType", "INT"}},
      {{"name", "strCol"}, {"dataType", "STRING"}},
      {{"name", "arrCol"}, {"dataType", "INT"}, {"singleValueField", false}},
  });
  schema["metricFieldSpecs"] = json::array({
      {{"name", "doubleCol"}, {"dataType", "DOUBLE"}},
  });
  schema["dateTimeFieldSpecs"] = json::array({
      {{"name", "tsCol"}, {"dataType", "TIMESTAMP"},
       {"format", "1:MILLISECONDS:EPOCH"}, {"granularity", "1:MILLISECONDS"}},
  });
  schema["primaryKeyColumns"] = json::array({"intCol"});
  return schema;
}

json airlineSchema() {
  json schema;
  schema["schemaName"] = "airlineStats";
  schema["dimensionFieldSpecs"] = json::array({
      {{"name", "Carrier"}, {"dataType", "STRING"}},
  });
  schema["metricFieldSpecs"] = json::array({
      {{"name", "ArrDelay"}, {"dataType", "INT"}},
  });
  schema["dateTimeFieldSpecs"] = json::array();
  return schema;
}

}  // namespace

MockBroker::MockBroker(bool requireAuth) : requireAuth_(requireAuth) {
  ensureSocketsInitialized();
  listenFd_ = static_cast<MockSocket>(::socket(AF_INET, SOCK_STREAM, 0));
  if (listenFd_ == kInvalidSocket) throw std::runtime_error("socket() failed");
  int one = 1;
  setsockopt(static_cast<decltype(::socket(0, 0, 0))>(listenFd_), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  auto fd = static_cast<decltype(::socket(0, 0, 0))>(listenFd_);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("bind() failed");
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  if (listen(fd, 16) != 0) throw std::runtime_error("listen() failed");
  thread_ = std::thread([this]() { run(); });
}

MockBroker::~MockBroker() {
  stop_ = true;
  if (listenFd_ != kInvalidSocket) {
    shutdownSocket(listenFd_);
    closeSocket(listenFd_);
  }
  if (thread_.joinable()) thread_.join();
}

std::string MockBroker::lastQueryOptions() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return lastQueryOptions_;
}

void MockBroker::run() {
  while (!stop_) {
    int rc = pollReadable(listenFd_, 100);
    if (rc <= 0) continue;
    MockSocket client = static_cast<MockSocket>(
        ::accept(static_cast<decltype(::socket(0, 0, 0))>(listenFd_), nullptr, nullptr));
    if (client == kInvalidSocket) continue;

    std::string request;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
      int n = recvSome(client, buf, sizeof(buf));
      if (n <= 0) break;
      request.append(buf, static_cast<size_t>(n));
      headerEnd = request.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) {
      closeSocket(client);
      continue;
    }

    // Parse request line and a couple of headers.
    std::string method, path;
    {
      size_t sp1 = request.find(' ');
      size_t sp2 = request.find(' ', sp1 + 1);
      method = request.substr(0, sp1);
      path = request.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    auto headerValue = [&request, headerEnd](const std::string& name) -> std::string {
      std::string lower;
      for (char c : request.substr(0, headerEnd)) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      std::string key = "\r\n" + name + ":";
      size_t pos = lower.find(key);
      if (pos == std::string::npos) return "";
      size_t start = pos + key.size();
      size_t end = lower.find("\r\n", start);
      if (end == std::string::npos) end = lower.size();
      std::string value = request.substr(start, end - start);
      size_t b = value.find_first_not_of(" \t");
      size_t e = value.find_last_not_of(" \t\r\n");
      if (b == std::string::npos) return "";
      return value.substr(b, e - b + 1);
    };

    size_t contentLength = 0;
    std::string cl = headerValue("content-length");
    if (!cl.empty()) contentLength = static_cast<size_t>(std::stoul(cl));
    std::string body = request.substr(headerEnd + 4);
    while (body.size() < contentLength) {
      int n = recvSome(client, buf, sizeof(buf));
      if (n <= 0) break;
      body.append(buf, static_cast<size_t>(n));
    }

    int status = 200;
    // Mimic real Pinot: /health is @Produces(text/plain), so a request that
    // only accepts application/json gets 406 Not Acceptable.
    std::string accept = headerValue("accept");
    std::string responseBody;
    if (path == "/health" && !accept.empty() && accept.find("*/*") == std::string::npos &&
        accept.find("text/plain") == std::string::npos) {
      status = 406;
      responseBody = "{\"error\":\"not acceptable\"}";
    } else {
      responseBody = handle(method, path, headerValue("authorization"), body, &status);
    }
    std::string statusText = (status == 200) ? "OK" : (status == 401 ? "Unauthorized" : "Error");
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + statusText +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(responseBody.size()) + "\r\nConnection: close\r\n\r\n" +
                           responseBody;
    sendAll(client, response.data(), response.size());
    closeSocket(client);
  }
}

std::string MockBroker::handle(const std::string& method, const std::string& path,
                               const std::string& authHeader, const std::string& body,
                               int* status) {
  if (requireAuth_ && authHeader != "Bearer secret") {
    *status = 401;
    return "{\"error\":\"unauthorized\"}";
  }
  if (method == "GET" && path == "/health") return "OK";
  if (method == "GET" && path == "/tables") {
    return "{\"tables\":[\"airlineStats\",\"testTable\"]}";
  }
  if (method == "GET" && path == "/tables/testTable/schema") return testTableSchema().dump();
  if (method == "GET" && path == "/tables/airlineStats/schema") return airlineSchema().dump();

  if (method == "POST" && path == "/query/sql") {
    json req = json::parse(body, nullptr, false);
    std::string sql = req.is_object() ? req.value("sql", "") : "";
    {
      std::lock_guard<std::mutex> lock(mtx_);
      lastQueryOptions_ = req.is_object() ? req.value("queryOptions", "") : "";
    }
    if (sql == "SELECT err") {
      // Pinot returns query errors with an exceptions array in the body.
      return errorResponse().dump();
    }
    if (sql == "SELECT options") {
      std::lock_guard<std::mutex> lock(mtx_);
      return echoResponse(lastQueryOptions_, "options").dump();
    }
    if (sql.find("FROM testTable") != std::string::npos && sql.rfind("SELECT *", 0) == 0) {
      return fullResultResponse().dump();
    }
    return echoResponse(sql, "echo").dump();
  }
  *status = 404;
  return "{\"error\":\"not found\"}";
}
