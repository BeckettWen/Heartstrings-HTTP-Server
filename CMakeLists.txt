cmake_minimum_required(VERSION 3.24)

project(Heartstrings LANGUAGES CXX)

if(APPLE)
    message(FATAL_ERROR "Use X-18.xcodeproj on macOS; CMake provides the non-Apple POSIX backend.")
endif()

if(NOT UNIX)
    message(FATAL_ERROR "The C++ handler test target currently requires a POSIX platform.")
endif()

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

enable_testing()

add_executable(heartstrings_handler_tests tests/handler_tests.cpp)
target_compile_features(heartstrings_handler_tests PRIVATE cxx_std_23)
target_include_directories(heartstrings_handler_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(heartstrings_handler_tests PRIVATE GTest::gtest_main Threads::Threads)

add_test(NAME heartstrings_handler_tests COMMAND heartstrings_handler_tests)

add_library(heartstrings STATIC NetworkingCore/Heartstrings.cpp)
target_compile_features(heartstrings PUBLIC cxx_std_23)
target_include_directories(heartstrings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(heartstrings PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(heartstrings_portable_server_tests tests/portable_server_tests.cpp)
target_compile_features(heartstrings_portable_server_tests PRIVATE cxx_std_23)
target_link_libraries(heartstrings_portable_server_tests PRIVATE heartstrings GTest::gtest_main)

add_test(NAME heartstrings_portable_server_tests COMMAND heartstrings_portable_server_tests)
