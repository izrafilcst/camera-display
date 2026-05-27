cmake_minimum_required(VERSION 3.16)
project(test_patterns_host CXX)

# Compile test harness + implementation together.
# ../test_patterns.cpp does NOT exist yet (RED state) — build will fail with
# a linker/compiler error until the Coder implements the functions.
add_executable(test_patterns
    test_patterns.cpp
    ../test_patterns.cpp
)

target_compile_options(test_patterns PRIVATE
    -std=c++17
    -Wall
    -Wextra
    -Werror
)

enable_testing()
add_test(NAME display_patterns COMMAND test_patterns)
