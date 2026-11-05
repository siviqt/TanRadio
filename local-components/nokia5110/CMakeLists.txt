idf_component_register(SRCS "src/nokia5110.c" "src/canvas.c" "src/espidf.c"
    INCLUDE_DIRS "include" PRIV_REQUIRES esp_driver_gpio esp_rom freertos)
