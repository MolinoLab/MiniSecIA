#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

typedef enum {
    TONO_INICIO,        // empieza a grabar
    TONO_FIN,           // grabación guardada en la cola
    TONO_OK,            // el servidor creó el elemento
    TONO_PENDIENTE,     // guardado en pendientes (no se entendió la acción)
    TONO_ERROR,         // el servidor rechazó el audio o falta configuración
    TONO_EN_COLA,       // sin conexión: se subirá más tarde
    TONO_CANCELADO,
    TONO_APAGADO,
    TONO_BATERIA_BAJA,
} tono_t;

bool audio_iniciar(i2c_master_bus_handle_t bus, int volumen, int ganancia_db);
void audio_apagar(void);

// Bloqueante. No suena mientras se graba (se oiría en la grabación).
void audio_tono(tono_t tono);
void audio_pitidos(int n);

// Graba a `ruta` (WAV 16 kHz mono 16 bits) hasta audio_grabar_parar o `max_seg`.
bool audio_grabar_iniciar(const char *ruta, int max_seg);

// Devuelve los bytes de audio guardados; 0 si se canceló o falló (y el fichero se borra).
uint32_t audio_grabar_parar(bool cancelar);

bool audio_grabando(void);
bool audio_grabacion_al_maximo(void);
