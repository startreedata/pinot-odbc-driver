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

// Standalone mock Pinot broker/controller for manual testing (e.g. isql).
// Prints the chosen port on stdout and serves until killed.

#include <unistd.h>

#include <cstdio>

#include "mock_broker.h"

int main() {
  MockBroker broker;
  std::printf("%d\n", broker.port());
  std::fflush(stdout);
  for (;;) pause();
  return 0;
}
