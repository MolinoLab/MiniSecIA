#include "meditacion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ajustes.h"
#include "audio.h"
#include "cliente.h"
#include "esp_log.h"
#include "realidad.h"
#include "reloj.h"

static const char *TAG = "meditacion";

#define CLAVE_ACT "med_act"
#define CLAVE_MODO "med_modo"
#define CLAVE_VECES "med_veces"
#define CLAVE_INTV "med_intv"
#define CLAVE_DUR "med_dur"
#define CLAVE_HI "med_hi"
#define CLAVE_HF "med_hf"
#define CLAVE_OFF "med_off"
#define CLAVE_EV "med_ev"
#define CLAVE_FIN "med_fin"
#define CLAVE_LAST "med_last"

#define MODO_VECES 0
#define MODO_INTERVALO 1

typedef struct {
    bool activo;
    int modo;
    int veces;
    int intervalo_min;
    int duracion_min;
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

void meditacion_iniciar(void)
{
    s_cfg.activo = estado_leer_u32(CLAVE_ACT, 0) != 0;
    s_cfg.modo = (int)estado_leer_u32(CLAVE_MODO, MODO_VECES);
    s_cfg.veces = (int)estado_leer_u32(CLAVE_VECES, 4);
    s_cfg.intervalo_min = (int)estado_leer_u32(CLAVE_INTV, 180);
    s_cfg.duracion_min = (int)estado_leer_u32(CLAVE_DUR, 10);
    s_cfg.hora_inicio = (int)estado_leer_u32(CLAVE_HI, 8);
    s_cfg.hora_fin = (int)estado_leer_u32(CLAVE_HF, 22);
    s_cfg.tz_offset_min = (int)estado_leer_u32(CLAVE_OFF, 60);
    s_cfg.veces = clamp_i(s_cfg.veces, 1, 24);
    s_cfg.intervalo_min = clamp_i(s_cfg.intervalo_min, 15, 24 * 60);
    s_cfg.duracion_min = clamp_i(s_cfg.duracion_min, 1, 120);
    s_cfg.hora_inicio = clamp_i(s_cfg.hora_inicio, 0, 23);
    s_cfg.hora_fin = clamp_i(s_cfg.hora_fin, 1, 24);
    if (s_cfg.hora_fin <= s_cfg.hora_inicio) {
        s_cfg.hora_fin = s_cfg.hora_inicio + 1;
    }
    ESP_LOGI(TAG, "activo=%d modo=%d veces=%d intv=%d dur=%d ventana=%d-%d off=%d",
             s_cfg.activo ? 1 : 0, s_cfg.modo, s_cfg.veces, s_cfg.intervalo_min, s_cfg.duracion_min,
             s_cfg.hora_inicio, s_cfg.hora_fin, s_cfg.tz_offset_min);
}

bool meditacion_activa(void)
{
    return s_cfg.activo;
}

static void guardar_cfg(void)
{
    estado_guardar_u32(CLAVE_ACT, s_cfg.activo ? 1 : 0);
    estado_guardar_u32(CLAVE_MODO, (uint32_t)s_cfg.modo);
    estado_guardar_u32(CLAVE_VECES, (uint32_t)s_cfg.veces);
    estado_guardar_u32(CLAVE_INTV, (uint32_t)s_cfg.intervalo_min);
    estado_guardar_u32(CLAVE_DUR, (uint32_t)s_cfg.duracion_min);
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

bool meditacion_sincronizar(void)
{
    char buf[512];
    int cod = cliente_get_texto("/meditacion/config", buf, sizeof(buf));
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
                n.veces = clamp_i(v, 1, 24);
            } else if (parse_int_linea(linea, "intervalo_min", &v)) {
                n.intervalo_min = clamp_i(v, 15, 24 * 60);
            } else if (parse_int_linea(linea, "duracion_min", &v)) {
                n.duracion_min = clamp_i(v, 1, 120);
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
    if (!s_cfg.activo) {
        estado_guardar_u32(CLAVE_FIN, 0);
    }
    ESP_LOGI(TAG, "Config sync: activo=%d modo=%d", s_cfg.activo ? 1 : 0, s_cfg.modo);
    return true;
}

static void local_tm(time_t utc, struct tm *out)
{
    time_t local = utc + (time_t)s_cfg.tz_offset_min * 60;
    gmtime_r(&local, out);
}

// Campos de reloj local → instante UTC (aprox.; suficiente con offset fijo del sync).
static time_t local_partes_a_utc(int year, int mon, int mday, int hour, int min)
{
    // Días desde 1970-01-01 con regla gregoriana simple.
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
    // Avanza usando medianoche UTC equivalente + 86400*dias.
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

static time_t proximo_inicio_utc(time_t ahora)
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

    int huecos[24];
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

uint32_t meditacion_segundos_hasta(void)
{
    if (!reloj_valido()) {
        return 0;
    }
    time_t ahora = time(NULL);
    uint32_t fin_at = estado_leer_u32(CLAVE_FIN, 0);
    if (fin_at > 0) {
        if ((time_t)fin_at <= ahora) {
            return 1;
        }
        return (uint32_t)((time_t)fin_at - ahora);
    }
    if (!s_cfg.activo) {
        return 0;
    }
    time_t prox = proximo_inicio_utc(ahora);
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

void meditacion_limpiar_evento(void)
{
    estado_guardar_u32(CLAVE_EV, TIMER_NADA);
}

uint32_t meditacion_preparar_sueno(uint32_t segundos_imagen)
{
    uint32_t seg = 0;
    timer_evento_t ev = TIMER_NADA;
    time_t ahora = time(NULL);

    // Una sesión en curso (voz o programada) siempre despierta al fin, aunque el
    // calendario de avisos esté pausado.
    if (reloj_valido()) {
        uint32_t fin_at = estado_leer_u32(CLAVE_FIN, 0);
        if (fin_at > 0) {
            uint32_t hasta_fin = ((time_t)fin_at <= ahora) ? 1 : (uint32_t)((time_t)fin_at - ahora);
            seg = hasta_fin;
            ev = TIMER_MEDITACION_FIN;
        } else if (s_cfg.activo) {
            time_t prox = proximo_inicio_utc(ahora);
            if (prox > 0) {
                uint32_t hasta = (prox <= ahora) ? 1 : (uint32_t)(prox - ahora);
                seg = hasta;
                ev = TIMER_MEDITACION_INICIO;
            }
        }
    }

    if (segundos_imagen > 0 && (seg == 0 || segundos_imagen < seg)) {
        seg = segundos_imagen;
        ev = TIMER_IMAGEN;
    }

    // Realidad: si es más cercana, gana el despertar (y limpia el evento de meditación).
    uint32_t seg_rea = realidad_segundos_hasta();
    if (seg_rea > 0 && (seg == 0 || seg_rea < seg)) {
        seg = seg_rea;
        ev = TIMER_NADA;
        realidad_armar_despertar(seg);
    } else {
        realidad_limpiar_evento();
    }

    estado_guardar_u32(CLAVE_EV, (uint32_t)ev);
    ESP_LOGI(TAG, "Sueño: %lu s evento=%d rea=%lu", (unsigned long)seg, (int)ev,
             (unsigned long)seg_rea);
    return seg;
}

void meditacion_lanzar_ahora(int duracion_min)
{
    int dur = duracion_min > 0 ? duracion_min : s_cfg.duracion_min;
    if (dur < 1) {
        dur = 10;
    }
    if (dur > 120) {
        dur = 120;
    }
    ESP_LOGI(TAG, "Inicio de meditación por voz (%d min)", dur);
    audio_tono(TONO_MEDITACION);
    time_t ahora = time(NULL);
    estado_guardar_u32(CLAVE_LAST, (uint32_t)ahora);
    estado_guardar_u32(CLAVE_FIN, (uint32_t)(ahora + (time_t)dur * 60));
}

void meditacion_al_despertar(void)
{
    timer_evento_t ev = (timer_evento_t)estado_leer_u32(CLAVE_EV, TIMER_NADA);
    estado_guardar_u32(CLAVE_EV, TIMER_NADA);
    if (ev == TIMER_MEDITACION_INICIO) {
        meditacion_lanzar_ahora(s_cfg.duracion_min);
    } else if (ev == TIMER_MEDITACION_FIN) {
        ESP_LOGI(TAG, "Fin de meditación");
        audio_tono(TONO_MEDITACION_FIN);
        estado_guardar_u32(CLAVE_FIN, 0);
    }
}
