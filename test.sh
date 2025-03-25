#!/bin/bash

# activate python's virtual env
. ./activate.sh --quiet
. .env 2>&1 >/dev/null || true

# check if we already build once
if [ ! -d "$build_folder" ]; then
  echo "ERROR: project is not built. run: './build.sh' and then rerun ./test.sh"
  exit 1
fi
ctest --test-dir $build_folder --output-on-failure $@
