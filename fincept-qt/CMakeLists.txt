cmake_minimum_required(VERSION 3.27)

project(FinceptTerminal VERSION 4.5.1 LANGUAGES C CXX)

# The isolated KEEFI application is the only product built by this source tree.
# Keep the established option name for existing build scripts.
option(KEEFI_G1_MODE "Build the isolated KEEFI native application" ON)
if(NOT KEEFI_G1_MODE)
    message(FATAL_ERROR "Only the isolated KEEFI build is available")
endif()
include(cmake/KeefiG1.cmake)
