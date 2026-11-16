#include "imagenes.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ajustes.h"
#include "almacen.h"
#include "cliente.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "pantalla.h"
#include "placa.h"
#include "reloj.h"

static const char *TAG = "imagenes";

#define MAX_IMAGENES 200
#define TAM_MANIFIESTO (16 * 1024)

static bool es_bin(const char *nombre)
{
    size_t n = strlen(nombre);
    return n > 4 && strcasecmp(nombre + n - 4, ".bin") == 0;
}

static int agregar_dir(const char *dir, char (*rutas)[RUTA_MAX], int n)
{
    DIR *d = opendir(dir);
    if (!d) {
        return n;
    }
    struct dirent *e;
    while (n < MAX_IMAGENES && (e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR && es_bin(e->d_name)) {
            snprintf(rutas[n], RUTA_MAX, "%s/%s", dir, e->d_name);
            if (almacen_tamano(rutas[n]) == EPD_BYTES) {
                n++;
            }
        }
    }
    closedir(d);
    return n;
}

static int comparar(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

bool imagenes_siguiente(void)
{
    char (*rutas)[RUTA_MAX] = heap_caps_malloc(MAX_IMAGENES * RUTA_MAX, MALLOC_CAP_SPIRAM);
    if (!rutas) {
        return false;
    }
    int n = agregar_dir(DIR_IMG_FIJAS, rutas, 0);
    n = agregar_dir(DIR_IMG_REM, rutas, n);
    bool ok = false;
    if (n > 0) {
        qsort(rutas, n, RUTA_MAX, comparar);
        uint32_t idx = (estado_leer_u32("img_idx", UINT32_MAX) + 1) % n;
        ok = pantalla_mostrar(rutas[idx]);
        estado_guardar_u32("img_idx", idx);
        if (reloj_valido()) {
            estado_guardar_u32("img_t", (uint32_t)time(NULL));
        }
    } else {
        ESP_LOGI(TAG, "No hay imágenes en la SD");
    }
    free(rutas);
    return ok;
}

uint32_t imagenes_segundos_hasta_rotacion(void)
{
    if (g_ajustes.imagen_horas <= 0 || !reloj_valido()) {
        return 0;
    }
    uint32_t ultima = estado_leer_u32("img_t", 0);
    uint32_t periodo = (uint32_t)g_ajustes.imagen_horas * 3600;
    uint32_t ahora = (uint32_t)time(NULL);
    if (ultima == 0 || ahora < ultima || ahora - ultima >= periodo) {
        return 1;
    }
    return periodo - (ahora - ultima);
}

void imagenes_rotar_si_toca(void)
{
    if (imagenes_segundos_hasta_rotacion() == 1) {
        imagenes_siguiente();
    }
}

static void hex(const uint8_t *h, char out[65])
{
    for (int i = 0; i < 32; i++) {
        sprintf(out + 2 * i, "%02x", h[i]);
    }
    out[64] = '\0';
}

static bool sha_fichero(const char *ruta, char out[65])
{
    uint8_t *buf = heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(ruta, "rb") : NULL;
    bool ok = false;
    if (f) {
        size_t n = fread(buf, 1, EPD_BYTES, f);
        fclose(f);
        if (n == EPD_BYTES) {
            uint8_t h[32];
            mbedtls_sha256(buf, n, h, 0);
            hex(h, out);
            ok = true;
        }
    }
    free(buf);
    return ok;
}

static bool id_valido(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 48) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool descargar(const char *id, const char *sha_esperado)
{
    char camino[96], parcial[RUTA_MAX], destino[RUTA_MAX];
    snprintf(camino, sizeof(camino), "/imagenes/%s.bin", id);
    snprintf(parcial, sizeof(parcial), "%s/%s.part", DIR_IMG_REM, id);
    snprintf(destino, sizeof(destino), "%s/%s.bin", DIR_IMG_REM, id);
    uint8_t h[32];
    char sha[65];
    long bytes = 0;
    int estado = cliente_descargar(camino, parcial, h, &bytes);
    hex(h, sha);
    if (estado != 200 || bytes != EPD_BYTES || strcmp(sha, sha_esperado) != 0) {
        ESP_LOGW(TAG, "Descarga de %s fallida (HTTP %d, %ld bytes)", id, estado, bytes);
        remove(parcial);
        return false;
    }
    remove(destino);
    return rename(parcial, destino) == 0;
}

bool imagenes_toca_sincronizar(void)
{
    uint32_t ahora = (uint32_t)time(NULL);
    uint32_t ultima = estado_leer_u32("sync_img_t", 0);
    return !(reloj_valido() && ultima && ahora >= ultima &&
             ahora - ultima < (uint32_t)g_ajustes.sync_imagenes_horas * 3600);
}

bool imagenes_sincronizar(bool forzar)
{
    if (!forzar && !imagenes_toca_sincronizar()) {
        return true;
    }
    char *manifiesto = heap_caps_malloc(TAM_MANIFIESTO, MALLOC_CAP_SPIRAM);
    if (!manifiesto) {
        return false;
    }
    int estado = cliente_get_texto("/imagenes/manifest?formato=txt", manifiesto, TAM_MANIFIESTO);
    if (estado != 200) {
        ESP_LOGW(TAG, "Manifiesto: HTTP %d", estado);
        free(manifiesto);
        return false;
    }

    // Ids presentes en el manifiesto, para borrar después las que sobran.
    char (*ids)[52] = heap_caps_calloc(MAX_IMAGENES, 52, MALLOC_CAP_SPIRAM);
    int n_ids = 0, nuevas = 0;
    bool todo_bien = ids != NULL;
    char *guardar = NULL;
    for (char *linea = strtok_r(manifiesto, "\n", &guardar); linea && ids;
         linea = strtok_r(NULL, "\n", &guardar)) {
        char id[64], sha[80];
        long bytes = 0;
        if (sscanf(linea, "%63s %79s %ld", id, sha, &bytes) != 3 || !id_valido(id) || strlen(sha) != 64 ||
            bytes != EPD_BYTES) {
            continue;
        }
        for (char *p = sha; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        if (n_ids < MAX_IMAGENES) {
            strlcpy(ids[n_ids++], id, 52);
        }
        char ruta[RUTA_MAX], actual[65];
        snprintf(ruta, sizeof(ruta), "%s/%s.bin", DIR_IMG_REM, id);
        if (sha_fichero(ruta, actual) && strcmp(actual, sha) == 0) {
            continue;
        }
        if (descargar(id, sha)) {
            nuevas++;
        } else {
            todo_bien = false;
        }
    }
    free(manifiesto);

    if (ids) {
        // Se recogen primero y se borran después de cerrar el directorio.
        char (*sobran)[RUTA_MAX] = heap_caps_malloc(MAX_IMAGENES * RUTA_MAX, MALLOC_CAP_SPIRAM);
        int n_sobran = 0;
        DIR *d = sobran ? opendir(DIR_IMG_REM) : NULL;
        struct dirent *e;
        while (d && n_sobran < MAX_IMAGENES && (e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) {
                continue;
            }
            char base[64];
            strlcpy(base, e->d_name, sizeof(base));
            char *punto = strrchr(base, '.');
            bool listado = false;
            if (punto && strcasecmp(punto, ".bin") == 0) {
                *punto = '\0';
                for (int i = 0; i < n_ids && !listado; i++) {
                    listado = strcmp(ids[i], base) == 0;
                }
            }
            if (!listado) {
                snprintf(sobran[n_sobran++], RUTA_MAX, "%s/%s", DIR_IMG_REM, e->d_name);
            }
        }
        if (d) {
            closedir(d);
        }
        for (int i = 0; i < n_sobran; i++) {
            remove(sobran[i]);
        }
        free(sobran);
        free(ids);
    }
    ESP_LOGI(TAG, "Imágenes sincronizadas: %d en el servidor, %d nuevas", n_ids, nuevas);
    if (todo_bien && reloj_valido()) {
        estado_guardar_u32("sync_img_t", (uint32_t)time(NULL));
    }
    return todo_bien;
}
