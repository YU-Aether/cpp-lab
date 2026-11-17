cmake_minimum_required(VERSION 3.28)
project(cpp_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(GTest REQUIRED)

add_executable(ring_buffer_test tests/ring_buffer_test.cpp)
target_include_directories(ring_buffer_test PRIVATE src)
target_link_libraries(ring_buffer_test PRIVATE GTest::gtest_main)

# -Werror 暂时不开：先让告警可见，等养成"零告警"习惯后再升级成错误
target_compile_options(ring_buffer_test PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(ring_buffer_test)
