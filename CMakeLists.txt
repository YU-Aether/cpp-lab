cmake_minimum_required(VERSION 3.28)
project(cpp_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 信号槽需要 moc 生成代码，必须开启 AUTOMOC。
set(CMAKE_AUTOMOC ON)

include(CTest)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test)

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

add_executable(epoll_echo_server_test tests/epoll_echo_server_test.cpp)
target_include_directories(epoll_echo_server_test PRIVATE src)
target_link_libraries(epoll_echo_server_test PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(epoll_echo_server_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(frame_codec_test tests/frame_codec_test.cpp)
target_include_directories(frame_codec_test PRIVATE src)
target_link_libraries(frame_codec_test PRIVATE GTest::gtest_main)
target_compile_options(frame_codec_test PRIVATE -Wall -Wextra -Wpedantic)


# 界面与网络客户端编译成静态库，应用和测试共用同一份实现。
add_library(plantpulse_ui STATIC
    src/main_window.cpp
    src/main_window.h
    src/echo_client.cpp
    src/echo_client.h
    src/frame_codec.h
    src/frame_source.cpp
    src/frame_source.h
    src/someip_codec.h
)
target_include_directories(plantpulse_ui PUBLIC src)
target_link_libraries(plantpulse_ui PUBLIC Qt6::Widgets PRIVATE Qt6::Network)
target_compile_options(plantpulse_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(plantpulse_app src/main.cpp)
target_link_libraries(plantpulse_app PRIVATE plantpulse_ui)

add_executable(plantpulse_server src/plantpulse_server.cpp)
target_include_directories(plantpulse_server PRIVATE src)
target_link_libraries(plantpulse_server PRIVATE Threads::Threads)
target_compile_options(plantpulse_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(main_window_test tests/main_window_test.cpp)
target_link_libraries(main_window_test PRIVATE plantpulse_ui GTest::gtest Qt6::Test)
target_compile_options(main_window_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(echo_client_test tests/echo_client_test.cpp)
target_link_libraries(echo_client_test PRIVATE plantpulse_ui GTest::gtest Qt6::Test Threads::Threads)
target_compile_options(echo_client_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(frame_source_test tests/frame_source_test.cpp)
target_link_libraries(frame_source_test PRIVATE
    plantpulse_ui
    GTest::gtest
    Qt6::Test
    Threads::Threads
)
target_compile_options(frame_source_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(someip_test tests/someip_test.cpp)
target_include_directories(someip_test PRIVATE src)
target_link_libraries(someip_test PRIVATE GTest::gtest_main)
target_compile_options(someip_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(someip_server_test tests/someip_server_test.cpp)
target_include_directories(someip_server_test PRIVATE src)
target_link_libraries(someip_server_test PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(someip_server_test PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(ring_buffer_test)
gtest_discover_tests(spsc_queue_test)
gtest_discover_tests(blocking_spsc_queue_test)
gtest_discover_tests(blocking_echo_server_test)
gtest_discover_tests(epoll_echo_server_test)
gtest_discover_tests(frame_codec_test)
gtest_discover_tests(main_window_test)
gtest_discover_tests(echo_client_test)
gtest_discover_tests(frame_source_test)
gtest_discover_tests(someip_test)
gtest_discover_tests(someip_server_test)