idf_component_register(SRCS "blink_example_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi esp_event nvs_flash esp_http_server json led_strip)