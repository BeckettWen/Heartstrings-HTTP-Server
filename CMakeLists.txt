cmake_minimum_required(VERSION 4.0)
project(XN-5A_Heartstrings-C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL FOR ZED: Generates compilation data for the code editor
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# 1. Look for the core header file in standard and Homebrew paths
find_path(asio_INCLUDE_DIR NAMES asio.hpp
    PATHS "/opt/homebrew/include" "/usr/local/include"
)

# 2. Check if it was found and handle the REQUIRED keyword gracefully
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(asio
    REQUIRED_VARS asio_INCLUDE_DIR
)

# 3. If found, generate the clean asio::asio package target
if(asio_FOUND AND NOT TARGET asio::asio)
  add_library(asio::asio INTERFACE IMPORTED)
  set_target_properties(asio::asio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${asio_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE" # Ensures it doesn't look for Boost
    )
endif()

find_package(GTest REQUIRED)

# Define your executable name and source files


add_executable(tests
    tests/tests.cpp
)

target_link_libraries(
    tests
    PRIVATE
    GTest::gtest_main
    GTest::gtest
    asio::asio
)

include(GoogleTest)
gtest_discover_tests(tests)
