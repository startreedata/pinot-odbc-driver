#!/usr/bin/env bash
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

# Brings up the Apache Pinot batch quickstart in Docker (same pattern as the
# other Pinot client integration tests), waits until the baseballStats table
# is queryable, then runs the ODBC live integration tests against it.
#
# Usage:
#   test/run_docker_integration.sh [path-to-live_integration_tests]
#
# Environment overrides:
#   PINOT_IMAGE      docker image       (default apachepinot/pinot:1.3.0)
#   CONTAINER_NAME   container name     (default pinot-odbc-quickstart)
#   BROKER_PORT      host broker port   (default 8000)
#   CONTROLLER_PORT  host controller port (default 9000)
#   READY_TIMEOUT    seconds to wait for the table (default 600)
#   KEEP_CLUSTER=1   leave the container running after the tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_BIN="${1:-${SCRIPT_DIR}/../build/live_integration_tests}"

PINOT_IMAGE="${PINOT_IMAGE:-apachepinot/pinot:1.3.0}"
CONTAINER_NAME="${CONTAINER_NAME:-pinot-odbc-quickstart}"
BROKER_PORT="${BROKER_PORT:-8000}"
CONTROLLER_PORT="${CONTROLLER_PORT:-9000}"
READY_TIMEOUT="${READY_TIMEOUT:-600}"

if [[ ! -x "$TEST_BIN" ]]; then
  echo "live_integration_tests binary not found at $TEST_BIN" >&2
  echo "Build it first: cmake --build build" >&2
  exit 1
fi

cleanup() {
  if [[ "${KEEP_CLUSTER:-0}" != "1" ]]; then
    echo "Tearing down container ${CONTAINER_NAME}..."
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  else
    echo "KEEP_CLUSTER=1 set; leaving ${CONTAINER_NAME} running."
  fi
}
trap cleanup EXIT

docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true

echo "Starting Pinot quickstart (${PINOT_IMAGE})..."
docker run -d --name "${CONTAINER_NAME}" \
  -p "${CONTROLLER_PORT}:9000" \
  -p "${BROKER_PORT}:8000" \
  "${PINOT_IMAGE}" QuickStart -type batch >/dev/null

echo -n "Waiting for baseballStats to become queryable"
deadline=$(( $(date +%s) + READY_TIMEOUT ))
until response=$(curl -s -m 5 -X POST -H 'Content-Type: application/json' \
        -d '{"sql":"SELECT count(*) FROM baseballStats"}' \
        "http://localhost:${BROKER_PORT}/query/sql" 2>/dev/null) \
      && echo "$response" | grep -q '"rows":\[\[' \
      && ! echo "$response" | grep -q '"errorCode"'; do
  if (( $(date +%s) > deadline )); then
    echo
    echo "Pinot quickstart did not become ready within ${READY_TIMEOUT}s" >&2
    docker logs --tail 50 "${CONTAINER_NAME}" >&2 || true
    exit 1
  fi
  if ! docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null | grep -q true; then
    echo
    echo "Pinot container exited unexpectedly" >&2
    docker logs --tail 50 "${CONTAINER_NAME}" >&2 || true
    exit 1
  fi
  echo -n "."
  sleep 5
done
echo " ready."

export PINOT_BROKER="localhost:${BROKER_PORT}"
export PINOT_CONTROLLER="localhost:${CONTROLLER_PORT}"
echo "Running ${TEST_BIN} against broker=${PINOT_BROKER} controller=${PINOT_CONTROLLER}"
"${TEST_BIN}"
