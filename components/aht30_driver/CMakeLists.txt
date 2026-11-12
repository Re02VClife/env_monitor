idf_component_register(
    SRCS "aht30.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c freertos
)
