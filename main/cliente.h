#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SUBIDA_OK,              // 200, X-Voz-Estado: ok|audio
    SUBIDA_PENDIENTE,       // 200, X-Voz-Estado: pendiente (guardado en pendientes.csv)
    SUBIDA_MEDITACION,      // 200, X-Voz-Estado: meditacion (tono local + cuenta atrás)
    SUBIDA_REALIDAD,        // 200, X-Voz-Estado: realidad (chirp local)
    SUBIDA_VACIA,           // 422 con X-Voz-Estado: vacio (sin voz); se borra
    SUBIDA_RECHAZADA,       // 413: demasiado grande; a /sdcard/fallidos
    SUBIDA_ERROR_CONFIG,    // 400/401/403: token o device_id mal configurados; se para la cola
    SUBIDA_REINTENTAR,      // red, 409, 5xx: se queda en la cola
} resultado_subida_t;

// POST /voz en multipart. `grabado_en` en ISO UTC o "" si el reloj no tenía hora.
// Si el servidor responde con audio efímero, escribe el path en `audio_path` (p. ej. /voz/respuesta/id.opus).
// Si es meditación, deja la duración en minutos en `meditacion_min` (>0) cuando no es NULL.
resultado_subida_t cliente_subir(const char *ruta, const char *request_id, const char *grabado_en,
                                 char *audio_path, size_t audio_path_n, int *meditacion_min);

// Baja el Opus de respuesta, lo reproduce, lo borra en local y hace ACK en el servidor.
bool cliente_reproducir_respuesta(const char *audio_path);

// GET de `camino` (p. ej. "/imagenes/manifest?formato=txt"). Devuelve el código HTTP
// (o -1) y deja el cuerpo, terminado en '\0', en `buf`.
int cliente_get_texto(const char *camino, char *buf, size_t n);

// GET a fichero. Devuelve el código HTTP (o -1); `sha256` recibe el hash de lo descargado
// y `bytes` su tamaño.
int cliente_descargar(const char *camino, const char *destino, uint8_t sha256[32], long *bytes);

// POST sin cuerpo (p. ej. /audios/{id}/ack). Devuelve el código HTTP o -1.
int cliente_post(const char *camino);
