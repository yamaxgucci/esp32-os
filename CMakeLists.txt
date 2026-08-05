# ArgonOS firmware image.
#
# Build:  idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)

set(ARGON_VERSION "0.1.0")

# The public ABI headers live outside components/ so that the application SDK
# and the kernel share exactly one copy of the contract.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(argonos VERSION ${ARGON_VERSION})
