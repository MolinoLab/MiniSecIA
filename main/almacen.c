#include "almacen.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "placa.h"
#include "sdmmc_cmd.h"

static const char *TAG = "almacen";
static sdmmc_card_t *s_tarjeta = NULL;

// Menos de ~0,3 s de audio: pulsación accidental, no se sube.
#define BYTES_MINIMOS (WAV_CABECERA + AUDIO_HZ * 2 * 3 / 10)

static void crear_dir(const char *ruta)
{
    if (mkdir(ruta, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %d", ruta, errno);
    }
}

bool almacen_montar(void)
{
    if (s_tarjeta) {
        return true;
    }
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(PUNTO_MONTAJE, &host, &slot, &cfg, &s_tarjeta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la microSD: %s", esp_err_to_name(err));
        s_tarjeta = NULL;
        return false;
    }
    crear_dir(DIR_COLA);
    crear_dir(DIR_FALLIDOS);
    crear_dir("/sdcard/img");
    crear_dir(DIR_IMG_FIJAS);
    crear_dir(DIR_IMG_REM);
    ESP_LOGI(TAG, "microSD montada (%llu MB)",
             ((unsigned long long)s_tarjeta->csd.capacity) * s_tarjeta->csd.sector_size / (1024 * 1024));
    return true;
}

void almacen_desmontar(void)
{
    if (s_tarjeta) {
        esp_vfs_fat_sdcard_unmount(PUNTO_MONTAJE, s_tarjeta);
        s_tarjeta = NULL;
    }
}

bool almacen_montada(void)
{
    return s_tarjeta != NULL;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

void wav_cabecera(uint8_t c[WAV_CABECERA], uint32_t bytes_datos)
{
    memcpy(c, "RIFF", 4);
    put_u32(c + 4, 36 + bytes_datos);
    memcpy(c + 8, "WAVEfmt ", 8);
    put_u32(c + 16, 16);
    put_u16(c + 20, 1);              // PCM
    put_u16(c + 22, 1);              // mono
    put_u32(c + 24, AUDIO_HZ);
    put_u32(c + 28, AUDIO_HZ * 2);   // bytes por segundo
    put_u16(c + 32, 2);              // alineación de bloque
    put_u16(c + 34, 16);             // bits
    memcpy(c + 36, "data", 4);
    put_u32(c + 40, bytes_datos);
}

long almacen_tamano(const char *ruta)
{
    struct stat st;
    return stat(ruta, &st) == 0 ? (long)st.st_size : -1;
}

static bool termina_en(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcasecmp(s + n - m, suf) == 0;
}

static void reparar_fichero(const char *ruta)
{
    long tam = almacen_tamano(ruta);
    if (tam < BYTES_MINIMOS) {
        ESP_LOGW(TAG, "Grabación vacía descartada: %s", ruta);
        remove(ruta);
        return;
    }
    uint32_t datos = (uint32_t)(tam - WAV_CABECERA) & ~1u;
    FILE *f = fopen(ruta, "r+b");
    if (!f) {
        return;
    }
    uint8_t cab[WAV_CABECERA];
    bool bien = fread(cab, 1, sizeof(cab), f) == sizeof(cab) && memcmp(cab, "RIFF", 4) == 0 &&
                cab[40] == (datos & 0xff) && cab[41] == ((datos >> 8) & 0xff) &&
                cab[42] == ((datos >> 16) & 0xff) && cab[43] == ((datos >> 24) & 0xff);
    if (!bien) {
        wav_cabecera(cab, datos);
        fseek(f, 0, SEEK_SET);
        fwrite(cab, 1, sizeof(cab), f);
        ESP_LOGW(TAG, "Cabecera reparada: %s (%lu bytes)", ruta, (unsigned long)datos);
    }
    fclose(f);
}

void almacen_reparar_cola(void)
{
    DIR *d = opendir(DIR_COLA);
    if (!d) {
        return;
    }
    struct dirent *e;
    char ruta[RUTA_MAX];
    char nueva[RUTA_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) {
            continue;
        }
        snprintf(ruta, sizeof(ruta), "%s/%s", DIR_COLA, e->d_name);
        if (termina_en(e->d_name, ".tmp")) {
            reparar_fichero(ruta);
            if (almacen_tamano(ruta) >= 0) {
                strlcpy(nueva, ruta, sizeof(nueva));
                memcpy(nueva + strlen(nueva) - 4, ".wav", 4);
                rename(ruta, nueva);
            }
        } else if (termina_en(e->d_name, ".wav")) {
            reparar_fichero(ruta);
        }
    }
    closedir(d);
}

int almacen_listar_cola(char rutas[][RUTA_MAX], int max, int *total)
{
    int n = 0, t = 0;
    char ruta[RUTA_MAX];
    DIR *d = opendir(DIR_COLA);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR || !termina_en(e->d_name, ".wav")) {
                continue;
            }
            t++;
            if (max <= 0) {
                continue;
            }
            snprintf(ruta, sizeof(ruta), "%s/%s", DIR_COLA, e->d_name);
            // Inserción ordenada conservando solo los `max` más antiguos.
            int pos = n;
            while (pos > 0 && strcmp(rutas[pos - 1], ruta) > 0) {
                pos--;
            }
            if (pos >= max) {
                continue;
            }
            int ultimo = n < max ? n : max - 1;
            for (int i = ultimo; i > pos; i--) {
                memcpy(rutas[i], rutas[i - 1], RUTA_MAX);
            }
            memcpy(rutas[pos], ruta, RUTA_MAX);
            if (n < max) {
                n++;
            }
        }
        closedir(d);
    }
    if (total) {
        *total = t;
    }
    return n;
}

int almacen_contar_cola(void)
{
    int total = 0;
    almacen_listar_cola(NULL, 0, &total);
    return total;
}

uint32_t almacen_max_secuencia(void)
{
    uint32_t max = 0;
    DIR *d = opendir(DIR_COLA);
    if (!d) {
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t s = (uint32_t)strtoul(e->d_name, NULL, 10);
        if (s > max) {
            max = s;
        }
    }
    closedir(d);
    return max;
}

bool almacen_mover_a_fallidos(const char *ruta)
{
    const char *nombre = strrchr(ruta, '/');
    char destino[RUTA_MAX];
    snprintf(destino, sizeof(destino), "%s/%s", DIR_FALLIDOS, nombre ? nombre + 1 : ruta);
    remove(destino);
    if (rename(ruta, destino) != 0) {
        ESP_LOGE(TAG, "No se pudo mover %s a fallidos; se borra", ruta);
        return remove(ruta) == 0;
    }
    return true;
}
