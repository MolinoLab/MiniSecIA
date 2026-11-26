#include "realidad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ajustes.h"
#include "audio.h"
#include "cliente.h"
#include "esp_log.h"
#include "reloj.h"

static const char *TAG = "realidad";

#define CLAVE_ACT "rea_act"
#define CLAVE_MODO "rea_modo"
#define CLAVE_VECES "rea_veces"
#define CLAVE_INTV "rea_intv"
#define CLAVE_HI "rea_hi"
#define CLAVE_HF "rea_hf"
#define CLAVE_OFF "rea_off"
#define CLAVE_EV "rea_ev"
#define CLAVE_LAST "rea_last"

#define MODO_VECES 0
#define MODO_INTERVALO 1

typedef struct {
    bool activo;
    int modo;
    int veces;
    int intervalo_min;
    int hora_inicio;
    int hora_fin;
    int tz_offset_min;
} cfg_t;

static cfg_t s_cfg;

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void realidad_iniciar(void)
{
    s_cfg.activo = estado_leer_u32(CLAVE_ACT, 0) != 0;
    s_cfg.modo = (int)estado_leer_u32(CLAVE_MODO, MODO_VECES);
    s_cfg.veces = (int)estado_leer_u32(CLAVE_VECES, 6);
    s_cfg.intervalo_min = (int)estado_leer_u32(CLAVE_INTV, 120);
    s_cfg.hora_inicio = (int)estado_leer_u32(CLAVE_HI, 8);
    s_cfg.hora_fin = (int)estado_leer_u32(CLAVE_HF, 22);
    s_cfg.tz_offset_min = (int)estado_leer_u32(CLAVE_OFF, 60);
    s_cfg.veces = clamp_i(s_cfg.veces, 1, 48);
    s_cfg.intervalo_min = clamp_i(s_cfg.intervalo_min, 15, 24 * 60);
    s_cfg.hora_inicio = clamp_i(s_cfg.hora_inicio, 0, 23);
    s_cfg.hora_fin = clamp_i(s_cfg.hora_fin, 1, 24);
    if (s_cfg.hora_fin <= s_cfg.hora_inicio) {
        s_cfg.hora_fin = s_cfg.hora_inicio + 1;
    }
    ESP_LOGI(TAG, "activo=%d modo=%d veces=%d intv=%d ventana=%d-%d off=%d",
             s_cfg.activo ? 1 : 0, s_cfg.modo, s_cfg.veces, s_cfg.intervalo_min,
             s_cfg.hora_inicio, s_cfg.hora_fin, s_cfg.tz_offset_min);
}

bool realidad_activa(void)
{
    return s_cfg.activo;
}

static void guardar_cfg(void)
{
    estado_guardar_u32(CLAVE_ACT, s_cfg.activo ? 1 : 0);
    estado_guardar_u32(CLAVE_MODO, (uint32_t)s_cfg.modo);
    estado_guardar_u32(CLAVE_VECES, (uint32_t)s_cfg.veces);
    estado_guardar_u32(CLAVE_INTV, (uint32_t)s_cfg.intervalo_min);
    estado_guardar_u32(CLAVE_HI, (uint32_t)s_cfg.hora_inicio);
    estado_guardar_u32(CLAVE_HF, (uint32_t)s_cfg.hora_fin);
    estado_guardar_u32(CLAVE_OFF, (uint32_t)s_cfg.tz_offset_min);
}

static bool parse_int_linea(const char *linea, const char *clave, int *out)
{
    size_t n = strlen(clave);
    if (strncmp(linea, clave, n) != 0 || linea[n] != '=') {
        return false;
    }
    *out = atoi(linea + n + 1);
    return true;
}

bool realidad_sincronizar(void)
{
    char buf[512];
    int cod = cliente_get_texto("/realidad/config", buf, sizeof(buf));
    if (cod != 200 || !buf[0]) {
        ESP_LOGW(TAG, "No se pudo bajar config (%d)", cod);
        return false;
    }
    cfg_t n = s_cfg;
    char *p = buf;
    while (*p) {
        char *fin = strchr(p, '\n');
        if (fin) {
            *fin = '\0';
        }
        char *linea = p;
        while (*linea == ' ' || *linea == '\r') {
            linea++;
        }
        if (*linea) {
            int v;
            if (parse_int_linea(linea, "activo", &v)) {
                n.activo = v != 0;
            } else if (strncmp(linea, "modo=", 5) == 0) {
                n.modo = (strcmp(linea + 5, "intervalo") == 0) ? MODO_INTERVALO : MODO_VECES;
            } else if (parse_int_linea(linea, "veces", &v)) {
                n.veces = clamp_i(v, 1, 48);
            } else if (parse_int_linea(linea, "intervalo_min", &v)) {
                n.intervalo_min = clamp_i(v, 15, 24 * 60);
            } else if (parse_int_linea(linea, "hora_inicio", &v)) {
                n.hora_inicio = clamp_i(v, 0, 23);
            } else if (parse_int_linea(linea, "hora_fin", &v)) {
                n.hora_fin = clamp_i(v, 1, 24);
            } else if (parse_int_linea(linea, "tz_offset_min", &v)) {
                n.tz_offset_min = clamp_i(v, -12 * 60, 14 * 60);
            }
        }
        if (!fin) {
            break;
        }
        p = fin + 1;
    }
    if (n.hora_fin <= n.hora_inicio) {
        n.hora_fin = n.hora_inicio + 1;
    }
    s_cfg = n;
    guardar_cfg();
    ESP_LOGI(TAG, "Config sync: activo=%d modo=%d", s_cfg.activo ? 1 : 0, s_cfg.modo);
    return true;
}

static void local_tm(time_t utc, struct tm *out)
{
    time_t local = utc + (time_t)s_cfg.tz_offset_min * 60;
    gmtime_r(&local, out);
}

static time_t local_partes_a_utc(int year, int mon, int mday, int hour, int min)
{
    int y = year;
    int m = mon;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m - 3) + 2) / 5 + mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int64_t secs = days * 86400 + hour * 3600 + min * 60;
    return (time_t)(secs - (int64_t)s_cfg.tz_offset_min * 60);
}

static void sumar_dias_local(struct tm *loc, int dias)
{
    time_t utc = local_partes_a_utc(loc->tm_year + 1900, loc->tm_mon + 1, loc->tm_mday, 12, 0);
    utc += (time_t)dias * 86400;
    local_tm(utc, loc);
}

static void huecos_minutos(int *out, int *n_out)
{
    int ini = s_cfg.hora_inicio * 60;
    int fin = s_cfg.hora_fin * 60;
    int n = s_cfg.veces;
    if (n < 1) {
        n = 1;
    }
    if (n > 48) {
        n = 48;
    }
    if (n == 1) {
        out[0] = ini;
        *n_out = 1;
        return;
    }
    int span = fin - ini;
    for (int i = 0; i < n; i++) {
        out[i] = ini + (span * i) / (n - 1);
    }
    *n_out = n;
}

static time_t proximo_utc(time_t ahora)
{
    if (!s_cfg.activo || !reloj_valido()) {
        return 0;
    }
    struct tm loc;
    local_tm(ahora, &loc);

    if (s_cfg.modo == MODO_INTERVALO) {
        uint32_t last = estado_leer_u32(CLAVE_LAST, 0);
        time_t cand = ahora + 60;
        if (last > 0) {
            time_t desde_last = (time_t)last + (time_t)s_cfg.intervalo_min * 60;
            if (desde_last > cand) {
                cand = desde_last;
            }
        }
        for (int intento = 0; intento < 4; intento++) {
            local_tm(cand, &loc);
            int mins = loc.tm_hour * 60 + loc.tm_min;
            int ini = s_cfg.hora_inicio * 60;
            int fin = s_cfg.hora_fin * 60;
            if (mins >= ini && mins < fin) {
                return cand;
            }
            if (mins < ini) {
                cand = local_partes_a_utc(loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
                                          s_cfg.hora_inicio, 0);
            } else {
                sumar_dias_local(&loc, 1);
                cand = local_partes_a_utc(loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
                                          s_cfg.hora_inicio, 0);
            }
        }
        return cand;
    }

    int huecos[48];
    int nh = 0;
    huecos_minutos(huecos, &nh);
    local_tm(ahora, &loc);
    for (int dia = 0; dia < 3; dia++) {
        struct tm dia_tm = loc;
        if (dia > 0) {
            sumar_dias_local(&dia_tm, dia);
        }
        for (int i = 0; i < nh; i++) {
            time_t t = local_partes_a_utc(dia_tm.tm_year + 1900, dia_tm.tm_mon + 1, dia_tm.tm_mday,
                                          huecos[i] / 60, huecos[i] % 60);
            if (t > ahora + 30) {
                return t;
            }
        }
    }
    return 0;
}

uint32_t realidad_segundos_hasta(void)
{
    if (!s_cfg.activo || !reloj_valido()) {
        return 0;
    }
    time_t ahora = time(NULL);
    time_t prox = proximo_utc(ahora);
    if (prox <= 0) {
        return 0;
    }
    if (prox <= ahora) {
        return 1;
    }
    time_t d = prox - ahora;
    if (d > 7 * 24 * 3600) {
        d = 7 * 24 * 3600;
    }
    return (uint32_t)d;
}

void realidad_limpiar_evento(void)
{
    estado_guardar_u32(CLAVE_EV, 0);
}

void realidad_armar_despertar(uint32_t segundos)
{
    (void)segundos;
    estado_guardar_u32(CLAVE_EV, 1);
    ESP_LOGI(TAG, "Armado despertar realidad");
}

void realidad_lanzar_ahora(void)
{
    ESP_LOGI(TAG, "Checkeo de realidad");
    audio_tono(TONO_REALIDAD);
    estado_guardar_u32(CLAVE_LAST, (uint32_t)time(NULL));
}

void realidad_al_despertar(void)
{
    if (estado_leer_u32(CLAVE_EV, 0) == 0) {
        return;
    }
    estado_guardar_u32(CLAVE_EV, 0);
    realidad_lanzar_ahora();
}
