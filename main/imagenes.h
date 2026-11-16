#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pasa a la siguiente imagen de /sdcard/img/fijas + /sdcard/img/remotas (orden alfabético).
bool imagenes_siguiente(void);

// Cambia de imagen si han pasado `imagen_horas` desde el último cambio.
void imagenes_rotar_si_toca(void);

// Segundos hasta el próximo cambio automático (0 si no hay cambio programado).
uint32_t imagenes_segundos_hasta_rotacion(void);

bool imagenes_toca_sincronizar(void);

// Descarga las imágenes nuevas del servidor y borra las que ya no están. Requiere red.
// Sin `forzar`, solo si han pasado `sync_imagenes_horas` desde la última vez.
bool imagenes_sincronizar(bool forzar);
