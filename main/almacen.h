#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DIR_COLA      "/sdcard/cola"
#define DIR_FALLIDOS  "/sdcard/fallidos"
#define DIR_IMG_FIJAS "/sdcard/img/fijas"
#define DIR_IMG_REM   "/sdcard/img/remotas"

#define RUTA_MAX 128
#define WAV_CABECERA 44

bool almacen_montar(void);
void almacen_desmontar(void);
bool almacen_montada(void);

// Cabecera WAV PCM 16 bits mono a AUDIO_HZ con `bytes_datos` de muestras.
void wav_cabecera(uint8_t cab[WAV_CABECERA], uint32_t bytes_datos);

// Corrige cabeceras de grabaciones cortadas (corte de batería) y renombra .tmp -> .wav.
void almacen_reparar_cola(void);

// Rutas de la cola en orden de grabación (los nombres empiezan por la secuencia).
// Devuelve cuántas ha escrito en `rutas` (máximo `max`); `total` recibe el número real.
int almacen_listar_cola(char rutas[][RUTA_MAX], int max, int *total);
int almacen_contar_cola(void);

// Secuencia más alta presente en la cola (0 si está vacía).
uint32_t almacen_max_secuencia(void);

bool almacen_mover_a_fallidos(const char *ruta);
long almacen_tamano(const char *ruta);
