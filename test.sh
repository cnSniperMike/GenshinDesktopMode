#!/bin/zsh
set -eu

project_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$project_dir"
test_dir='.build/tests'
mkdir -p "$test_dir"

test_sources=(
    genshin/*_test.cpp
    tests/character_pointer_state_test.cpp
    tests/coarse_scroll_state_test.cpp
    tests/codex_pointer_state_test.cpp
    tests/control_device_policy_test.cpp
    tests/control_hotswap_state_test.cpp
    tests/interaction_wheel_router_test.cpp
    tests/keyboard_sources_test.cpp
    tests/map_touch_routing_test.cpp
    tests/map_touch_state_test.cpp
    tests/mouse_drag_state_test.cpp
    tests/native_original_motion_state_test.cpp
    tests/presentation_policy_test.cpp
    tests/ps_audio_signal_test.cpp
    tests/scroll_router_test.cpp
    tests/scroll_state_test.cpp
    tests/web_escape_policy_test.cpp
)

for source in "${test_sources[@]}"; do
    name="${source:t:r}"
    xcrun clang++ -std=c++17 -O2 "$source" -o "$test_dir/$name"
    "$test_dir/$name"
done
printf 'Passed %s tests.\n' "${#test_sources[@]}"
