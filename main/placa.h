// Pines de la Waveshare ESP32-S3-ePaper-1.54G (wiki y ejemplos ESP-IDF 5.5.1 de Waveshare).
#pragma once

#include "driver/gpio.h"

// Alimentación
#define PIN_EPD_PWR      GPIO_NUM_6   // 0 = pantalla alimentada
#define PIN_AUDIO_PWR    GPIO_NUM_42  // 0 = códec y amplificador alimentados
#define PIN_VBAT_PWR     GPIO_NUM_17  // 1 = mantener la batería conectada (enclavamiento)
#define PIN_LED          GPIO_NUM_3   // 0 = LED verde encendido
#define PIN_BATERIA_ADC  GPIO_NUM_4   // ADC1 canal 3, divisor 1:2

// Botones (activos a nivel bajo)
#define PIN_BOTON_BOOT   GPIO_NUM_0
#define PIN_BOTON_PWR    GPIO_NUM_18

// I2C: ES8311 (0x18), PCF85063 (0x51), SHTC3 (0x70)
#define PIN_I2C_SDA      GPIO_NUM_47
#define PIN_I2C_SCL      GPIO_NUM_48
#define DIR_RTC          0x51

// I2S hacia el ES8311 (el amplificador lo gobierna el driver del códec)
#define PIN_I2S_MCLK     GPIO_NUM_14
#define PIN_I2S_BCLK     GPIO_NUM_15
#define PIN_I2S_WS       GPIO_NUM_38
#define PIN_I2S_DOUT     GPIO_NUM_45
#define PIN_I2S_DIN      GPIO_NUM_16
#define PIN_PA           GPIO_NUM_46

// e-Paper 1,54" (G) por SPI3, CS manual
#define PIN_EPD_DC       GPIO_NUM_10
#define PIN_EPD_CS       GPIO_NUM_11
#define PIN_EPD_SCK      GPIO_NUM_12
#define PIN_EPD_MOSI     GPIO_NUM_13
#define PIN_EPD_RST      GPIO_NUM_9
#define PIN_EPD_BUSY     GPIO_NUM_8   // 1 = libre

// microSD SDMMC de 1 bit
#define PIN_SD_CLK       GPIO_NUM_39
#define PIN_SD_CMD       GPIO_NUM_41
#define PIN_SD_D0        GPIO_NUM_40

#define EPD_ANCHO        200
#define EPD_ALTO         200
#define EPD_BYTES        (EPD_ANCHO * EPD_ALTO / 4)

#define AUDIO_HZ         16000
#define PUNTO_MONTAJE    "/sdcard"
