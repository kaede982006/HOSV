#!/usr/bin/env sh
set -eu

cmake -S . -B build -DSEEDANCE2_USE_BUNDLED_DEPS=ON
cmake --build build --config Release

