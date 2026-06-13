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
project(pinot_odbc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PINOT_ODBC_BUILD_TESTS "Build unit and integration tests" ON)
option(PINOT_ODBC_DOCKER_TESTS
  "Register the Docker-based live Pinot quickstart test with ctest" OFF)

find_package(CURL REQUIRED)

# ODBC SDK headers (unixODBC or iODBC on POSIX; the Windows SDK provides
# them on Windows). We only need sql.h/sqlext.h/sqlucode.h; the driver
# manager links against us, not the other way around.
if(NOT WIN32)
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

  # SQLGetPrivateProfileString (DSN lookup) is resolved at runtime via
  # dlsym/dlopen from the driver manager's installer library, so there is no
  # link-time odbcinst dependency on POSIX.
else()
  # On Windows, SQLGetPrivateProfileString (registry-backed DSNs) lives in
  # odbccp32 from the Windows SDK. Its static dllload.obj references
  # _vsnwprintf_s, which the UCRT provides via legacy_stdio_definitions.
  set(ODBCINST_LIBRARY odbccp32 legacy_stdio_definitions)
endif()

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
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(ODBC_INCLUDE_DIR)
  target_include_directories(pinot_odbc_core PUBLIC ${ODBC_INCLUDE_DIR})
endif()
target_link_libraries(pinot_odbc_core PUBLIC CURL::libcurl ${CMAKE_DL_LIBS})
if(MSVC)
  # /utf-8: sources contain UTF-8 string literals (unit tests).
  # /FI win_compat.h: windows.h must be included before sql.h everywhere.
  # PUBLIC so test targets that compile against the driver headers inherit it.
  target_compile_options(pinot_odbc_core PUBLIC
    /utf-8 "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/win_compat.h")
endif()

add_library(pinot_odbc SHARED $<TARGET_OBJECTS:pinot_odbc_core>)
target_link_libraries(pinot_odbc PRIVATE CURL::libcurl ${CMAKE_DL_LIBS})
if(WIN32)
  # Export the ODBC entry points from the DLL; odbccp32 provides
  # SQLGetPrivateProfileString (registry-backed DSNs).
  target_sources(pinot_odbc PRIVATE src/pinot_odbc.def)
  target_link_libraries(pinot_odbc PRIVATE ${ODBCINST_LIBRARY})
endif()
set_target_properties(pinot_odbc PROPERTIES OUTPUT_NAME "pinot_odbc")

# ---- installation & packaging -------------------------------------------------

include(GNUInstallDirs)
install(TARGETS pinot_odbc
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/pinot-odbc-driver)
install(FILES
  examples/odbcinst.ini.sample
  examples/odbc.ini.sample
  examples/pyodbc_example.py
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/pinot-odbc-driver/examples)

set(CPACK_PACKAGE_NAME "pinot-odbc-driver")
if(PINOT_ODBC_PACKAGE_VERSION)
  set(CPACK_PACKAGE_VERSION ${PINOT_ODBC_PACKAGE_VERSION})
else()
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
endif()
set(CPACK_PACKAGE_VENDOR "StarTree Inc.")
set(CPACK_PACKAGE_CONTACT "StarTree Inc. <support@startree.ai>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ODBC driver for Apache Pinot")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/startreedata/pinot-odbc-driver")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "database")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4 | libcurl4t64")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "unixodbc")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "StarTree Community License")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")
include(CPack)

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
  if(WIN32)
    target_link_libraries(integration_tests PRIVATE ws2_32)
  endif()
  add_test(NAME integration_tests COMMAND integration_tests)

  # Standalone mock Pinot server for manual driver-manager testing (isql etc.).
  add_executable(mock_pinot_server test/mock_server_main.cpp test/mock_broker.cpp)
  target_include_directories(mock_pinot_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
  if(WIN32)
    target_link_libraries(mock_pinot_server PRIVATE ws2_32)
  endif()

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
