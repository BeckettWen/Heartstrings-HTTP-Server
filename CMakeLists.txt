cmake_minimum_required(VERSION 4.0)
project(XN-5A_Heartstrings-C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL FOR ZED: Generates compilation data for the code editor
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

find_package(GTest REQUIRED)

# Define your executable name and source files
add_executable(XN-5A_Heartstrings-C
  main.cpp
  Http_Handling/HttpParser.hpp
  Http_Handling/HttpSerializer.h
  NetworkingCore/Lunafilament.hpp
)

add_executable(unit-tests
    tests/tests.cpp
)

target_link_libraries(
    unit-tests
    PRIVATE
    GTest::gtest_main
    GTest::gtest
)

include(GoogleTest)
gtest_discover_tests(unit-tests)
