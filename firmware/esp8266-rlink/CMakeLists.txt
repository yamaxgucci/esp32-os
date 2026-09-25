# ESP8266 RLINK coprocessor firmware.
#
# Build with ESP8266_RTOS_SDK (NOT ESP-IDF): set IDF_PATH to the SDK, source its
# export, then `idf.py build`.  See README.md.  The result is an ESP8266 image
# that speaks the RLINK wire protocol (apps/common/rlink/ag_rlink.h) over UART0,
# so an ArgonOS board flashes it with ESPFLASH.AXE and drives it via EXTRADIO.SYS
# (`net use extradio`).
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rlink8266)
