#include "botones.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "placa.h"

#define PERIODO_MS 10
#define ANTIRREBOTE_MS 30
#define LARGO_BOOT_MS 1500
#define LARGO_PWR_MS 2500

typedef struct {
    gpio_num_t pin;
    int largo_ms;
    evento_boton_t ev_corto, ev_largo;
    bool armado;      // PWR: no actuar hasta verlo suelto una vez
    bool pulsado;
    bool largo_emitido;
    int estable_ms;   // tiempo con la lectura contraria al estado actual
    int64_t desde_us;
} boton_t;

static QueueHandle_t s_cola;

static void emitir(evento_boton_t ev)
{
    xQueueSend(s_cola, &ev, 0);
}

static void actualizar(boton_t *b)
{
    bool lectura = gpio_get_level(b->pin) == 0;
    if (!b->armado) {
        if (!lectura) {
            b->armado = true;
            b->pulsado = false;
        }
        return;
    }
    if (lectura != b->pulsado) {
        b->estable_ms += PERIODO_MS;
        if (b->estable_ms < ANTIRREBOTE_MS) {
            return;
        }
        b->estable_ms = 0;
        b->pulsado = lectura;
        if (lectura) {
            b->desde_us = esp_timer_get_time();
            b->largo_emitido = false;
        } else if (!b->largo_emitido) {
            emitir(b->ev_corto);
        }
        return;
    }
    b->estable_ms = 0;
    if (b->pulsado && !b->largo_emitido &&
        (esp_timer_get_time() - b->desde_us) / 1000 >= b->largo_ms) {
        b->largo_emitido = true;
        emitir(b->ev_largo);
    }
}

static boton_t s_boot = {
    .pin = PIN_BOTON_BOOT, .largo_ms = LARGO_BOOT_MS,
    .ev_corto = EV_BOOT_CORTO, .ev_largo = EV_BOOT_LARGO, .armado = true,
};
static boton_t s_pwr = {
    .pin = PIN_BOTON_PWR, .largo_ms = LARGO_PWR_MS,
    .ev_corto = EV_PWR_CORTO, .ev_largo = EV_PWR_LARGO, .armado = false,
};

static void tarea_botones(void *arg)
{
    for (;;) {
        actualizar(&s_boot);
        actualizar(&s_pwr);
        vTaskDelay(pdMS_TO_TICKS(PERIODO_MS));
    }
}

void botones_iniciar(bool despertado_por_boot)
{
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << PIN_BOTON_BOOT) | (1ULL << PIN_BOTON_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    s_cola = xQueueCreate(8, sizeof(evento_boton_t));
    if (gpio_get_level(PIN_BOTON_BOOT) == 0) {
        s_boot.pulsado = true;
        s_boot.desde_us = esp_timer_get_time();
    } else if (despertado_por_boot) {
        // Pulsación breve que ya terminó mientras arrancaba.
        emitir(EV_BOOT_CORTO);
    }
    xTaskCreate(tarea_botones, "botones", 2560, NULL, 6, NULL);
}

QueueHandle_t botones_cola(void)
{
    return s_cola;
}
