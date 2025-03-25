#!/bin/bash

# activate python's virtual env
. ./activate.sh --quiet
. .env 2>&1 >/dev/null || true

# make sure we built the project
if [ ! -f "$cli_path" ]; then
  echo "Error: Executable not found at $cli_path"
  echo "Please build the project first by running: ./build.sh"
  exit 1
fi

exec $driver "$cli_path" "$@"
