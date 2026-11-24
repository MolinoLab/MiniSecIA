#pragma once

#include <stdbool.h>

// Baja los Ogg Opus pendientes del servidor (con Wi-Fi ya conectado).
void escucha_sincronizar(void);

// BOOT largo: la escucha más reciente, también las ya oídas. Sin Wi-Fi.
void escucha_empezar(void);

// Durante la reproducción: la anterior, o la misma si ya es la primera.
void escucha_anterior(void);

// Durante la reproducción: la siguiente. Si ya es la última, para.
void escucha_siguiente(void);

void escucha_parar(void);

bool escucha_activa(void);
