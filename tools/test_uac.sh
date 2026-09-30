#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/usb-audio360-tests.XXXXXX)
trap 'rm -f "$test_dir/uac_tests" "$test_dir/clock_tests" "$test_dir/setup_policy_tests" "$test_dir/cleanup_tests" "$test_dir/ownership_tests" "$test_dir/isoch_tests" "$test_dir/budget_tests" "$test_dir/pacer_tests" "$test_dir/deadline_tests" "$test_dir/tone_tests" "$test_dir/pipeline_tests" "$test_dir/debug_tests"; rmdir "$test_dir"' EXIT
compiler=${CXX:-clang++}
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/debug_command_test.cpp" \
  -o "$test_dir/debug_tests"
"$test_dir/debug_tests"
rm -f "$test_dir/debug_tests"
"$compiler" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/src/uac_descriptors.cpp" \
  "$project_root/tests/uac_descriptors_test.cpp" -o "$test_dir/uac_tests"
"$test_dir/uac_tests"
"$compiler" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/src/uac_descriptors.cpp" \
  "$project_root/src/uac_clock.cpp" "$project_root/tests/uac_clock_test.cpp" \
  -o "$test_dir/clock_tests"
"$test_dir/clock_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/uac_setup_policy_test.cpp" \
  -o "$test_dir/setup_policy_tests"
"$test_dir/setup_policy_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/cleanup_lifecycle_test.cpp" \
  -o "$test_dir/cleanup_tests"
"$test_dir/cleanup_tests"
"$compiler" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/transfer_ownership_test.cpp" \
  -o "$test_dir/ownership_tests"
"$test_dir/ownership_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/isoch_result_test.cpp" \
  -o "$test_dir/isoch_tests"
"$test_dir/isoch_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/stream_test_budget_test.cpp" \
  -o "$test_dir/budget_tests"
"$test_dir/budget_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/feedback_pacer_test.cpp" \
  -o "$test_dir/pacer_tests"
"$test_dir/pacer_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/playback_pacer_test.cpp" \
  -o "$test_dir/playback_pacer_tests"
"$test_dir/playback_pacer_tests"
rm -f "$test_dir/playback_pacer_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/control_deadline_test.cpp" \
  -o "$test_dir/deadline_tests"
"$test_dir/deadline_tests"
"$compiler" -std=c++98 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/tests/test_tone_test.cpp" \
  -o "$test_dir/tone_tests"
"$test_dir/tone_tests"
"$compiler" -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$project_root/src" "$project_root/src/uac_descriptors.cpp" \
  "$project_root/src/uac_clock.cpp" "$project_root/tests/pcm_pipeline_test.cpp" \
  -o "$test_dir/pipeline_tests"
"$test_dir/pipeline_tests"
