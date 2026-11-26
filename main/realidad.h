#pragma once

#include <stdbool.h>
#include <stdint.h>

void realidad_iniciar(void);

// Baja GET /realidad/config y lo guarda en NVS.
bool realidad_sincronizar(void);

bool realidad_activa(void);

// Segundos hasta el próximo checkeo (0 = no hay).
uint32_t realidad_segundos_hasta(void);

void realidad_limpiar_evento(void);

// Marca despertar por realidad tras `segundos` (y limpia eventos de meditación).
void realidad_armar_despertar(uint32_t segundos);

// Chirp inmediato (voz «realidad» / «checkeo»).
void realidad_lanzar_ahora(void);

void realidad_al_despertar(void);
