idf_component_register(
    SRCS
        "main.cpp"
        # Portable game core, also built by test/Makefile on the host
        "core/canvas.cpp"
        "core/fonts.cpp"
        "core/game.cpp"
        "core/link.cpp"
        "core/rules.cpp"
        "core/screens.cpp"
        "core/sha256.cpp"
        # reTerminal Sticky hardware
        "esp/board.cpp"
        "esp/epd.cpp"
        "esp/inputs.cpp"
        "esp/radio.cpp"
        "esp/touch.cpp"
    INCLUDE_DIRS "core" "esp"
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
)
