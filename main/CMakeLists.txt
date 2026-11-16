idf_component_register(
    SRCS
        "main.c"
        "ajustes.c"
        "almacen.c"
        "audio.c"
        "botones.c"
        "cliente.c"
        "energia.c"
        "imagenes.c"
        "pantalla.c"
        "red.c"
        "reloj.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_spi
        esp_event
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        fatfs
        mbedtls
        nvs_flash
        sdmmc
)

# GCC 14 trata -Wformat-truncation como error con -Werror=all; las rutas
# de la SD usan d_name (hasta 255) y RUTA_MAX 128 con snprintf acotado.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
