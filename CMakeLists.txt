# ArgonOS firmware image.
#
# Build:  idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)

set(ARGON_VERSION "0.1.0")

# The public ABI headers live outside components/ so that the application SDK
# and the kernel share exactly one copy of the contract.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components")

# components/fatfs overrides ESP-IDF's own, and an overriding component has to
# bring the Kconfig with it or every CONFIG_FATFS_* symbol disappears.  It is
# copied rather than committed so that it tracks the IDF in use - and it is
# copied *here*, before project.cmake, because that is what runs the Kconfig
# generator.  Doing it in the component's own CMakeLists is one configure pass
# too late: the first build in a fresh clone then fails on
# `'CONFIG_FATFS_VOLUME_COUNT' undeclared`, and the second succeeds, which is
# the kind of failure that gets blamed on anything but the build system.
if(DEFINED ENV{IDF_PATH})
    file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/fatfs/Kconfig" ARGON_FATFS_KCONFIG)
    file(COPY "${ARGON_FATFS_KCONFIG}"
         DESTINATION "${CMAKE_CURRENT_LIST_DIR}/components/fatfs")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(argonos VERSION ${ARGON_VERSION})
