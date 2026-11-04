# ESP8266 stand-in for a PCF8574 with a Mega Drive pad on it.  Build with
# ESP8266_RTOS_SDK (NOT ESP-IDF), from a copy with no space in its path - see
# README.md.
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(segapad8266)
