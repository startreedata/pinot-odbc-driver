# Copyright 2026 StarTree Inc.
#
# Licensed under the StarTree Community License (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy
# of the License at http://www.startree.ai/startree-community-license
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(pinot_odbc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PINOT_ODBC_BUILD_TESTS "Build unit and integration tests" ON)
option(PINOT_ODBC_DOCKER_TESTS
  "Register the Docker-based live Pinot quickstart test with ctest" OFF)

find_package(CURL REQUIRED)

# ODBC SDK headers (unixODBC or iODBC). We only need sql.h/sqlext.h/sqlucode.h;
# the driver manager links against us, not the other way around.
find_path(ODBC_INCLUDE_DIR sql.h
  HINTS
    /opt/homebrew/include
    /usr/local/include
    /usr/local/opt/unixodbc/include
    /usr/include)
if(NOT ODBC_INCLUDE_DIR)
  message(FATAL_ERROR
    "ODBC SDK headers (sql.h) not found. Install unixODBC: "
    "'brew install unixodbc' on macOS or 'apt-get install unixodbc-dev' on Debian/Ubuntu.")
endif()

# libodbcinst provides SQLGetPrivateProfileString for reading DSN entries
# from odbc.ini. Optional: without it, only full connection strings work.
find_library(ODBCINST_LIBRARY NAMES odbcinst
  HINTS
    /opt/homebrew/lib
    /usr/local/lib
    /usr/local/opt/unixodbc/lib)

set(PINOT_ODBC_CORE_SOURCES
  src/config.cpp
  src/types.cpp
  src/result_set.cpp
  src/pinot_client.cpp
  src/unicode_conv.cpp
  src/convert.cpp
  src/api_helpers.cpp
  src/odbc_handles.cpp
  src/odbc_connect.cpp
  src/odbc_stmt.cpp
  src/odbc_catalog.cpp
  src/odbc_unicode.cpp)

add_library(pinot_odbc_core OBJECT ${PINOT_ODBC_CORE_SOURCES})
set_target_properties(pinot_odbc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pinot_odbc_core PUBLIC
  ${ODBC_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pinot_odbc_core PUBLIC CURL::libcurl)
if(ODBCINST_LIBRARY)
  target_compile_definitions(pinot_odbc_core PRIVATE HAVE_ODBCINST=1)
endif()

add_library(pinot_odbc SHARED $<TARGET_OBJECTS:pinot_odbc_core>)
target_link_libraries(pinot_odbc PRIVATE CURL::libcurl)
if(ODBCINST_LIBRARY)
  target_link_libraries(pinot_odbc PRIVATE ${ODBCINST_LIBRARY})
endif()
set_target_properties(pinot_odbc PROPERTIES OUTPUT_NAME "pinot_odbc")

install(TARGETS pinot_odbc LIBRARY DESTINATION lib)

if(PINOT_ODBC_BUILD_TESTS)
  enable_testing()

  add_executable(unit_tests test/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE pinot_odbc_core)
  if(ODBCINST_LIBRARY)
    target_link_libraries(unit_tests PRIVATE ${ODBCINST_LIBRARY})
  endif()
  add_test(NAME unit_tests COMMAND unit_tests)

  add_executable(integration_tests test/integration_tests.cpp test/mock_broker.cpp)
  target_link_libraries(integration_tests PRIVATE pinot_odbc_core)
  if(ODBCINST_LIBRARY)
    target_link_libraries(integration_tests PRIVATE ${ODBCINST_LIBRARY})
  endif()
  add_test(NAME integration_tests COMMAND integration_tests)

  # Standalone mock Pinot server for manual driver-manager testing (isql etc.).
  add_executable(mock_pinot_server test/mock_server_main.cpp test/mock_broker.cpp)
  target_include_directories(mock_pinot_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

  # Live tests against a real Pinot cluster (batch quickstart). The binary is
  # always built; run it via test/run_docker_integration.sh, which starts the
  # quickstart in Docker, or point PINOT_BROKER/PINOT_CONTROLLER at an
  # existing cluster.
  add_executable(live_integration_tests test/live_integration_tests.cpp)
  target_link_libraries(live_integration_tests PRIVATE pinot_odbc_core)
  if(ODBCINST_LIBRARY)
    target_link_libraries(live_integration_tests PRIVATE ${ODBCINST_LIBRARY})
  endif()
  if(PINOT_ODBC_DOCKER_TESTS)
    add_test(NAME docker_integration_tests
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_docker_integration.sh
              $<TARGET_FILE:live_integration_tests>)
    set_tests_properties(docker_integration_tests PROPERTIES
      TIMEOUT 900
      LABELS "docker")
  endif()
endif()
