cmake_minimum_required(VERSION 3.10)
project(nostrc)

# Options to enable/disable NIPs and NSON
include(${CMAKE_SOURCE_DIR}/NipOptions.cmake)
option(ENABLE_NSON "Enable NSON compilation" OFF)

add_subdirectory(libgo)
add_subdirectory(libnostr)
add_subdirectory(libjson)

# Add the tests
enable_testing()
add_subdirectory(tests)

