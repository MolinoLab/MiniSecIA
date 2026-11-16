#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DESPERTAR_ENCENDIDO,  // arranque normal (botón PWR o USB)
    DESPERTAR_BOOT,       // botón BOOT durante el sueño profundo: grabar
    DESPERTAR_PWR,        // botón PWR durante el sueño profundo
    DESPERTAR_TEMPORIZADOR,
} despertar_t;

typedef enum {
    LED_APAGADO,
    LED_FIJO,
    LED_PARPADEO,
} modo_led_t;

// Lo primero en app_main: enclava la batería y libera los pines retenidos durante el sueño.
despertar_t energia_iniciar(void);

void energia_audio(bool encendido);
void energia_pantalla(bool encendido);
void energia_led(modo_led_t modo);

// Tensión de la batería en mV (0 si no se puede medir).
int energia_bateria_mv(void);

// Duerme hasta pulsar un botón o hasta `segundos` (0 = sin temporizador). No vuelve.
void energia_dormir(uint32_t segundos);

// Corta la batería. Con USB conectado la placa sigue alimentada: entonces duerme sin temporizador.
void energia_apagar(void);
