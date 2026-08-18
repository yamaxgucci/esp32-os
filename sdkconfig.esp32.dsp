# ArgonOS - the ESP32 board with no radios and the largest possible arena.
#
#   argon target esp32-dsp
#
# An overlay on sdkconfig.defaults.esp32 for measuring rather than for using:
# no Wi-Fi, no Bluetooth, and the 48 KB of instruction RAM they were holding
# handed back to the code arena, so that an image up to that size runs from RAM
# instead of through the flash cache.
#
# Which matters for exactly one thing so far, and it is the reason this project
# has a board at all: CKTBENCH is 36 KB of code, and timing a circuit solver
# that is executing through a cache measures the cache.

CONFIG_ARGON_ENABLE_NET=n
CONFIG_ARGON_NET_WIFI=n
CONFIG_ARGON_ENABLE_BLE=n
CONFIG_BT_ENABLED=n

CONFIG_ARGON_APP_ARENA_KB=48
