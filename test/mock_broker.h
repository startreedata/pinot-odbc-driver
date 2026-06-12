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

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// A minimal in-process HTTP server that mimics the Pinot broker
// (POST /query/sql, GET /health) and controller (GET /tables,
// GET /tables/{name}/schema) endpoints for integration tests.
class MockBroker {
 public:
  // When requireAuth is true, every request must carry
  // "Authorization: Bearer secret" or it is rejected with 401.
  explicit MockBroker(bool requireAuth = false);
  ~MockBroker();

  int port() const { return port_; }

  // queryOptions string received with the most recent query request.
  std::string lastQueryOptions() const;

 private:
  void run();
  std::string handle(const std::string& method, const std::string& path,
                     const std::string& authHeader, const std::string& body, int* status);

  int listenFd_ = -1;
  int port_ = 0;
  bool requireAuth_ = false;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mtx_;
  std::string lastQueryOptions_;
};
