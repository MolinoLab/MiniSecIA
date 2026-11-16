#pragma once

#include <stdbool.h>

void pantalla_iniciar(void);

// Encola la imagen .bin (200x200, 2 bits/píxel, 10000 bytes). Vuelve enseguida;
// el refresco completo tarda ~20 s.
bool pantalla_mostrar(const char *ruta_bin);

bool pantalla_ocupada(void);
