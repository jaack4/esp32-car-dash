#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
task_build_dir=$(mktemp -d /tmp/cardash-effects-test.XXXXXX)
for test in reverb_test tone_test; do
  c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I tests/stubs -I src "tests/$test.cpp" src/effects_output.cpp \
    -o "$task_build_dir/$test"
  "$task_build_dir/$test"
done
