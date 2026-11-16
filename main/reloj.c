#include "reloj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "placa.h"

static const char *TAG = "reloj";
static i2c_master_dev_handle_t s_rtc = NULL;

// 2025-01-01: cualquier hora anterior es un RTC sin poner en hora.
#define EPOCH_MINIMA 1735689600

static uint8_t bcd_a_bin(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0f);
}

static uint8_t bin_a_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static bool leer_registros(uint8_t reg, uint8_t *datos, size_t n)
{
    return s_rtc && i2c_master_transmit_receive(s_rtc, &reg, 1, datos, n, 100) == ESP_OK;
}

static bool escribir_registros(uint8_t reg, const uint8_t *datos, size_t n)
{
    uint8_t buf[16];
    if (!s_rtc || n + 1 > sizeof(buf)) {
        return false;
    }
    buf[0] = reg;
    memcpy(buf + 1, datos, n);
    return i2c_master_transmit(s_rtc, buf, n + 1, 100) == ESP_OK;
}

void reloj_iniciar(i2c_master_bus_handle_t bus)
{
    setenv("TZ", "UTC0", 1);
    tzset();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DIR_RTC,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_rtc) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo añadir el RTC al bus I2C");
        s_rtc = NULL;
        return;
    }

    // Control_1: reloj en marcha (STOP=0), modo 24 h.
    uint8_t ctrl1 = 0;
    if (leer_registros(0x00, &ctrl1, 1) && (ctrl1 & ((1 << 5) | (1 << 1)))) {
        ctrl1 &= ~((1 << 5) | (1 << 1));
        escribir_registros(0x00, &ctrl1, 1);
    }

    if (reloj_valido()) {
        return;  // Despertar de sueño profundo: el RTC interno conserva la hora.
    }
    uint8_t r[7];
    if (!leer_registros(0x04, r, sizeof(r))) {
        ESP_LOGW(TAG, "No se pudo leer el RTC");
        return;
    }
    if (r[0] & 0x80) {
        ESP_LOGW(TAG, "El RTC perdió la hora (bit OS); se espera a SNTP");
        return;
    }
    struct tm t = {
        .tm_sec = bcd_a_bin(r[0] & 0x7f),
        .tm_min = bcd_a_bin(r[1] & 0x7f),
        .tm_hour = bcd_a_bin(r[2] & 0x3f),
        .tm_mday = bcd_a_bin(r[3] & 0x3f),
        .tm_mon = bcd_a_bin(r[5] & 0x1f) - 1,
        .tm_year = bcd_a_bin(r[6]) + 100,
    };
    time_t epoch = mktime(&t);  // TZ=UTC0: mktime interpreta UTC
    if (epoch < EPOCH_MINIMA) {
        ESP_LOGW(TAG, "Hora del RTC no creíble");
        return;
    }
    struct timeval tv = {.tv_sec = epoch};
    settimeofday(&tv, NULL);
    char iso[24];
    reloj_iso(epoch, iso, sizeof(iso));
    ESP_LOGI(TAG, "Hora desde el RTC: %s", iso);
}

bool reloj_valido(void)
{
    return time(NULL) >= EPOCH_MINIMA;
}

void reloj_guardar_rtc(void)
{
    if (!reloj_valido()) {
        return;
    }
    time_t ahora = time(NULL);
    struct tm t;
    gmtime_r(&ahora, &t);
    uint8_t r[7] = {
        bin_a_bcd(t.tm_sec),  // escribir los segundos borra el bit OS
        bin_a_bcd(t.tm_min),
        bin_a_bcd(t.tm_hour),
        bin_a_bcd(t.tm_mday),
        (uint8_t)t.tm_wday,
        bin_a_bcd(t.tm_mon + 1),
        bin_a_bcd(t.tm_year % 100),
    };
    if (escribir_registros(0x04, r, sizeof(r))) {
        ESP_LOGI(TAG, "RTC actualizado");
    }
}

void reloj_iso(time_t t, char *buf, size_t n)
{
    if (t < EPOCH_MINIMA) {
        buf[0] = '\0';
        return;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void reloj_compacto(time_t t, char *buf, size_t n)
{
    if (t < EPOCH_MINIMA) {
        strlcpy(buf, "0", n);
        return;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%Y%m%dT%H%M%SZ", &tm);
}

void reloj_compacto_a_iso(const char *c, char *buf, size_t n)
{
    // 20261110T153000Z -> 2026-11-10T15:30:00Z
    if (!c || strlen(c) < 16 || c[8] != 'T') {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, n, "%.4s-%.2s-%.2sT%.2s:%.2s:%.2sZ", c, c + 4, c + 6, c + 9, c + 11, c + 13);
}
