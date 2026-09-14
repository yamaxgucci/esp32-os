# The RLINK codec (ag_rlink.c/.h) is shared verbatim with the guest driver and
# the host tests - the tree at apps/common/rlink is the one source of truth.
# ESP8266_RTOS_SDK (v3.4) rejects component SRCS outside the component dir, so a
# copy is kept here (gitignored); refresh it with `tools/sync-codec.sh` after any
# change to apps/common/rlink.
idf_component_register(
    SRCS "rlink_main.c" "ag_rlink.c"
    INCLUDE_DIRS "."
    REQUIRES esp8266 nvs_flash lwip esp_event tcpip_adapter pthread)
