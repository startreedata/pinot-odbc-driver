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

#include <cstdio>
#include <sstream>
#include <string>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

template <typename A, typename B>
void checkEqImpl(const A& a, const B& b, const char* aexpr, const char* bexpr, const char* file,
                 int line) {
  g_checks++;
  if (!(a == b)) {
    std::ostringstream os;
    os << "FAIL " << file << ":" << line << ": " << aexpr << " == " << bexpr << " (got '" << a
       << "' vs '" << b << "')";
    std::printf("%s\n", os.str().c_str());
    g_failures++;
  }
}

#define CHECK_EQ(a, b) checkEqImpl((a), (b), #a, #b, __FILE__, __LINE__)

inline int testExitCode(const char* suiteName) {
  if (g_failures == 0) {
    std::printf("%s: all %d checks passed\n", suiteName, g_checks);
    return 0;
  }
  std::printf("%s: %d of %d checks FAILED\n", suiteName, g_failures, g_checks);
  return 1;
}
