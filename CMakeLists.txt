cmake_minimum_required(VERSION 3.28)
project(cpp_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(ring_buffer_test tests/ring_buffer_test.cpp)
target_include_directories(ring_buffer_test PRIVATE src)
target_link_libraries(ring_buffer_test PRIVATE GTest::gtest_main)
target_compile_options(ring_buffer_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(spsc_queue_test tests/spsc_queue_test.cpp)
target_include_directories(spsc_queue_test PRIVATE src)
target_link_libraries(spsc_queue_test PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(spsc_queue_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(blocking_spsc_queue_test tests/blocking_spsc_queue_test.cpp)
target_include_directories(blocking_spsc_queue_test PRIVATE src)
target_link_libraries(blocking_spsc_queue_test PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(blocking_spsc_queue_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(blocking_echo_server_test tests/blocking_echo_server_test.cpp)
target_include_directories(blocking_echo_server_test PRIVATE src)
target_link_libraries(blocking_echo_server_test PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(blocking_echo_server_test PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(ring_buffer_test)
gtest_discover_tests(spsc_queue_test)
gtest_discover_tests(blocking_spsc_queue_test)
gtest_discover_tests(blocking_echo_server_test)