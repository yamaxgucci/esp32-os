# ag_segapad.c/.h are copies of apps/common/segapad (gitignored): the SDK
# rejects sources outside the component directory.  AG_SEGAPAD_FN puts the
# pad model in IRAM, because the bus is answered from an interrupt.
idf_component_register(
    SRCS "segapad_main.c" "ag_segapad.c"
    INCLUDE_DIRS "."
    REQUIRES esp8266 nvs_flash lwip esp_event tcpip_adapter esp_http_server pthread app_update)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "AG_SEGAPAD_FN=__attribute__((section(\".iram1.segapad\")))")
