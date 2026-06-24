cmake_minimum_required(VERSION 4.0)
project(XN-5A_Heartstrings-C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL FOR ZED: Generates compilation data for the code editor
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Define your executable name and source files
add_executable(XN-5A_Heartstrings-C
  main.cpp
  Http_Handling/HttpParser.hpp
  Http_Handling/HttpSerializer.h
)

include(GoogleTest)
