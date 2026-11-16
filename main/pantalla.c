// Controlador del e-Paper 1,54" (G) de 4 colores, según el ejemplo 09_E_Paper_Test de Waveshare.
#include "pantalla.h"

#include <stdio.h>
#include <string.h>

#include "almacen.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "energia.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "placa.h"

static const char *TAG = "pantalla";

#define TROZO_SPI 1000
#define BUSY_TIMEOUT_MS 45000

static spi_device_handle_t s_spi = NULL;
static QueueHandle_t s_cola = NULL;
static volatile bool s_ocupada = false;
static uint8_t *s_imagen = NULL;   // PSRAM
static uint8_t *s_trozo = NULL;    // RAM interna con DMA

static void enviar(const uint8_t *datos, size_t n)
{
    spi_transaction_t t = {0};
    t.length = n * 8;
    t.tx_buffer = datos;
    spi_device_polling_transmit(s_spi, &t);
}

static void comando(uint8_t c)
{
    gpio_set_level(PIN_EPD_DC, 0);
    gpio_set_level(PIN_EPD_CS, 0);
    s_trozo[0] = c;
    enviar(s_trozo, 1);
    gpio_set_level(PIN_EPD_CS, 1);
}

static void dato(uint8_t d)
{
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    s_trozo[0] = d;
    enviar(s_trozo, 1);
    gpio_set_level(PIN_EPD_CS, 1);
}

static void datos(const uint8_t *d, size_t n)
{
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    while (n > 0) {
        size_t k = n < TROZO_SPI ? n : TROZO_SPI;
        memcpy(s_trozo, d, k);
        enviar(s_trozo, k);
        d += k;
        n -= k;
    }
    gpio_set_level(PIN_EPD_CS, 1);
}

static bool esperar_libre(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int ms = 0; ms < BUSY_TIMEOUT_MS; ms += 50) {
        if (gpio_get_level(PIN_EPD_BUSY)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "La pantalla no responde (BUSY)");
    return false;
}

static void iniciar_bus(void)
{
    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_EPD_MOSI,
        .sclk_io_num = PIN_EPD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TROZO_SPI,
    };
    spi_device_interface_config_t dev = {
        .spics_io_num = -1,
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    gpio_config_t sal = {
        .pin_bit_mask = (1ULL << PIN_EPD_RST) | (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&sal);
    gpio_config_t ent = {.pin_bit_mask = 1ULL << PIN_EPD_BUSY, .mode = GPIO_MODE_INPUT};
    gpio_config(&ent);
}

static bool encender_e_iniciar(void)
{
    energia_pantalla(true);
    gpio_set_level(PIN_EPD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    static const uint8_t BTST[] = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};
    comando(0x4D);
    dato(0x78);
    comando(0x00);  // PSR
    dato(0x0F);
    dato(0x29);
    comando(0x06);  // BTST_P
    datos(BTST, sizeof(BTST));
    comando(0x50);  // CDI
    dato(0x37);
    comando(0x61);  // resolución
    dato(EPD_ANCHO / 256);
    dato(EPD_ANCHO % 256);
    dato(EPD_ALTO / 256);
    dato(EPD_ALTO % 256);
    comando(0xE9);
    dato(0x01);
    comando(0x30);
    dato(0x08);
    comando(0x04);  // power on
    return esperar_libre();
}

static void dormir_y_apagar(void)
{
    comando(0x02);  // power off
    dato(0x00);
    esperar_libre();
    comando(0x07);  // deep sleep
    dato(0xA5);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Sin niveles altos hacia un panel sin alimentación.
    gpio_set_level(PIN_EPD_CS, 0);
    gpio_set_level(PIN_EPD_DC, 0);
    gpio_set_level(PIN_EPD_RST, 0);
    energia_pantalla(false);
}

static bool leer_imagen(const char *ruta)
{
    FILE *f = fopen(ruta, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(s_imagen, 1, EPD_BYTES, f);
    fclose(f);
    return n == EPD_BYTES;
}

static void tarea_pantalla(void *arg)
{
    char ruta[RUTA_MAX];
    for (;;) {
        if (xQueueReceive(s_cola, ruta, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Si se han encolado varias, solo cuenta la última.
        while (xQueueReceive(s_cola, ruta, 0) == pdTRUE) {
        }
        if (!leer_imagen(ruta)) {
            ESP_LOGE(TAG, "Imagen no válida: %s", ruta);
            s_ocupada = false;
            continue;
        }
        ESP_LOGI(TAG, "Mostrando %s", ruta);
        if (encender_e_iniciar()) {
            comando(0x10);
            datos(s_imagen, EPD_BYTES);
            comando(0x12);  // refresco
            dato(0x00);
            esperar_libre();
        }
        dormir_y_apagar();
        s_ocupada = uxQueueMessagesWaiting(s_cola) > 0;
    }
}

void pantalla_iniciar(void)
{
    s_imagen = heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
    s_trozo = heap_caps_malloc(TROZO_SPI, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_cola = xQueueCreate(2, RUTA_MAX);
    if (!s_imagen || !s_trozo || !s_cola) {
        ESP_LOGE(TAG, "Sin memoria");
        return;
    }
    iniciar_bus();
    xTaskCreate(tarea_pantalla, "pantalla", 4096, NULL, 3, NULL);
}

bool pantalla_mostrar(const char *ruta_bin)
{
    if (!s_cola) {
        return false;
    }
    char ruta[RUTA_MAX];
    strlcpy(ruta, ruta_bin, sizeof(ruta));
    s_ocupada = true;
    if (xQueueSend(s_cola, ruta, 0) != pdTRUE) {
        xQueueReset(s_cola);
        xQueueSend(s_cola, ruta, 0);
    }
    return true;
}

bool pantalla_ocupada(void)
{
    return s_ocupada;
}
