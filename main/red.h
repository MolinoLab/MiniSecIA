#pragma once

#include <stdbool.h>

// Enciende el Wi-Fi, elige la red configurada con mejor señal y espera IP.
// Si no hay hora válida (o hace más de un día que no se sincroniza), la toma por SNTP y la guarda en el RTC.
bool red_conectar(void);

// Apaga el Wi-Fi (ahorro de batería).
void red_desconectar(void);
