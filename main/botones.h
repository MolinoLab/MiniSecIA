#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    EV_BOOT_CORTO,
    EV_BOOT_LARGO,   // >= 1,5 s, se emite sin esperar a soltar
    EV_PWR_CORTO,
    EV_PWR_LARGO,    // >= 2,5 s
} evento_boton_t;

// Crea la tarea de lectura. Un botón que ya está pulsado al arrancar cuenta desde ese momento
// (BOOT que despierta del sueño), salvo PWR, que se ignora hasta soltarlo (es el de encendido).
// Con `despertado_por_boot` y BOOT ya suelto, se emite una pulsación corta.
void botones_iniciar(bool despertado_por_boot);

QueueHandle_t botones_cola(void);
