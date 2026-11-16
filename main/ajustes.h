#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AJUSTES_MAX_WIFI 3

typedef struct {
    char ssid[33];
    char pass[65];
} red_wifi_t;

typedef struct {
    red_wifi_t wifi[AJUSTES_MAX_WIFI];
    int n_wifi;
    char url[128];
    char token[96];
    char device_id[32];
    int volumen;
    int ganancia_mic;
    int max_grabacion_seg;
    int reposo_seg;
    int reintento_min;
    int imagen_horas;
    int sync_imagenes_horas;
    int timeout_http_seg;
} ajustes_t;

extern ajustes_t g_ajustes;

// Carga NVS (o valores de Kconfig). Si existe /sdcard/config.txt lo importa a NVS y lo borra.
void ajustes_cargar(bool sd_montada);

bool ajustes_servidor_configurado(void);

// Pequeño almacén de estado persistente (contadores, marcas de tiempo).
uint32_t estado_leer_u32(const char *clave, uint32_t defecto);
void estado_guardar_u32(const char *clave, uint32_t valor);
