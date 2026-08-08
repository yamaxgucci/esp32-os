# Override ESP-IDF's fatfs component so we can patch subtype detection in ff.c.
# All other sources still come from IDF_PATH; only ff.c is rewritten at configure.
#
# Why: stock ChaN FatFs picks FAT12/16/32 only from cluster count.  A correctly
# labeled small FAT16 volume (argon sync) was mounted as FAT12 → broken reads.

idf_build_get_property(target IDF_TARGET)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "fatfs override: IDF_PATH is not set")
endif()

# Forward slashes: backslashes break idf_component_register when the project
# path contains spaces (e.g. "Unity Projects").
file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/fatfs" FATFS_IDF)
set(FF_IN  "${FATFS_IDF}/src/ff.c")
set(FF_OUT "${CMAKE_CURRENT_BINARY_DIR}/ff_argon.c")

file(COPY "${FATFS_IDF}/Kconfig" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}")

get_filename_component(_argon_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(TO_CMAKE_PATH "${_argon_root}/tools/patch_fatfs_bpb.py" _ff_patch)
file(TO_CMAKE_PATH "${FF_IN}" FF_IN)
file(TO_CMAKE_PATH "${FF_OUT}" FF_OUT)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${_ff_patch}" "${FF_IN}" "${FF_OUT}"
    RESULT_VARIABLE _ff_patch_rc
)
if(NOT _ff_patch_rc EQUAL 0)
    message(FATAL_ERROR "fatfs override: failed to patch ff.c (rc=${_ff_patch_rc})")
endif()

set(srcs
    "${FF_OUT}"
    "${FATFS_IDF}/diskio/diskio.c"
    "${FATFS_IDF}/diskio/diskio_rawflash.c"
    "${FATFS_IDF}/diskio/diskio_wl.c"
    "${FATFS_IDF}/src/ffunicode.c"
)

set(include_dirs
    "${FATFS_IDF}/diskio"
    "${FATFS_IDF}/src"
)

set(requires wear_levelling)
set(priv_requires "")

if(${target} STREQUAL "linux")
    list(APPEND srcs "${FATFS_IDF}/port/linux/ffsystem.c")
else()
    list(APPEND srcs
        "${FATFS_IDF}/port/freertos/ffsystem.c"
        "${FATFS_IDF}/diskio/diskio_sdmmc.c"
        "${FATFS_IDF}/vfs/vfs_fat.c"
        "${FATFS_IDF}/vfs/vfs_fat_sdmmc.c"
        "${FATFS_IDF}/vfs/vfs_fat_spiflash.c"
    )
    list(APPEND include_dirs "${FATFS_IDF}/vfs")
    list(APPEND requires sdmmc esp_driver_sdmmc esp_driver_sdspi)
    set(priv_requires vfs esp_driver_gpio)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES ${priv_requires})
