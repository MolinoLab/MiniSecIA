#pragma once

#include <stdbool.h>
#include <stdint.h>

// Evento programado en el temporizador de sueño profundo.
typedef enum {
    TIMER_NADA = 0,
    TIMER_IMAGEN = 1,
    TIMER_MEDITACION_INICIO = 2,
    TIMER_MEDITACION_FIN = 3,
} timer_evento_t;

void meditacion_iniciar(void);

// Baja GET /meditacion/config y lo guarda en NVS. true si hubo respuesta usable.
bool meditacion_sincronizar(void);

bool meditacion_activa(void);

// Segundos hasta el próximo inicio o fin de meditación (0 = no hay).
uint32_t meditacion_segundos_hasta(void);

// Elige el próximo despertar entre imagen, meditación y realidad; guarda el motivo en NVS.
uint32_t meditacion_preparar_sueno(uint32_t segundos_imagen);

void meditacion_limpiar_evento(void);

// Tras DESPERTAR_TEMPORIZADOR: toca el tono que corresponda y programa el fin si aplica.
void meditacion_al_despertar(void);

// Inicio inmediato (voz «meditación»): tono de inicio y programa el fin a `duracion_min`.
// Si duracion_min <= 0 usa la configurada en NVS.
void meditacion_lanzar_ahora(int duracion_min);
