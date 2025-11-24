idf_component_register(SRCS "main.c"
INCLUDE_DIRS "."
REQUIRES bt
REQUIRES nvs_flash
REQUIRES esp_http_client
REQUIRES mqtt
REQUIRES esp_driver_gpio)
