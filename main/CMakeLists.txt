idf_component_register(
    SRCS "main.c" "probe.c"
    # esp_driver_gpio for the board probe, which talks to pins before the
    # kernel exists and therefore cannot go through the port layer.
    REQUIRES argon_kernel esp_driver_gpio)
