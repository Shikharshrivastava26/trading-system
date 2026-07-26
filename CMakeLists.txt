cmake_minimum_required(VERSION 3.16)
project(matching-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- static library ---
add_library(matching_engine_lib STATIC
    src/order_book.cpp
    src/matching_engine.cpp
    src/price_collar.cpp
)
target_include_directories(matching_engine_lib PUBLIC include)

# --- scratch test (Phase 1 exit criterion) ---
add_executable(scratch_test tests/scratch_test.cpp)
target_link_libraries(scratch_test PRIVATE matching_engine_lib)

# --- GoogleTest (wired up now, tests added in Phase 2) ---
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(unit_tests tests/test_matching_engine.cpp)
target_link_libraries(unit_tests PRIVATE matching_engine_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)

add_executable(threaded_tests tests/test_threaded_runner.cpp)
target_link_libraries(threaded_tests PRIVATE matching_engine_lib GTest::gtest_main pthread)
gtest_discover_tests(threaded_tests)

add_executable(benchmark benchmarks/benchmark_queue.cpp)
target_link_libraries(benchmark PRIVATE matching_engine_lib pthread)

add_executable(server src/server_main.cpp src/tcp_feed_handler.cpp)
target_link_libraries(server PRIVATE matching_engine_lib pthread)
