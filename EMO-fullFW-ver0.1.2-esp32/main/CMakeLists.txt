idf_component_register(
    SRCS
        "app_main.c"
        # K210 link: transport, coordinator, boot
        "k210_uart.c"
        "k210_spi.c"
        "k210_link.c"
        "k210_boot.c"
        # robot functions
        "microphone.c"
        "speaker.c"
        "video.c"
        "servo.c"
        "legs.c"
        "battery.c"
        "k210_controls.c"
        # network
        "net.c"
        "settings.c"
        "host_stream.c"
        "http_server.c"
        "rest_api.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html" "web/style.css" "web/app.js"
    REQUIRES
        nvs_flash
        esp_driver_gpio
        esp_driver_uart
        esp_driver_spi
        esp_ringbuf
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        esp_hw_support
        esp_adc
        esp_driver_ledc
        spiffs
        espressif__cjson
)
