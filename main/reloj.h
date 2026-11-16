#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "driver/i2c_master.h"

// Lee el PCF85063 (guardado en UTC) y pone en hora el sistema si el RTC no perdió la hora.
void reloj_iniciar(i2c_master_bus_handle_t bus);

bool reloj_valido(void);

// Escribe la hora del sistema en el RTC (tras sincronizar por SNTP).
void reloj_guardar_rtc(void);

// "2026-11-10T15:30:00Z", o "" si no hay hora válida.
void reloj_iso(time_t t, char *buf, size_t n);

// "20261110T153000Z", o "0" si no hay hora válida (para nombres de fichero).
void reloj_compacto(time_t t, char *buf, size_t n);

// Convierte el formato compacto a ISO ("" si es "0" o no se entiende).
void reloj_compacto_a_iso(const char *compacto, char *buf, size_t n);
