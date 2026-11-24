#include "escucha.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "ajustes.h"
#include "almacen.h"
#include "audio.h"
#include "cliente.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "escucha";

#define MAX_AUDIOS 32
#define TAM_MANIFIESTO (8 * 1024)

typedef enum {
    ACC_NINGUNA = 0,
    ACC_ANTERIOR,
    ACC_SIGUIENTE,
    ACC_ULTIMA,
    ACC_PARAR,
} accion_t;

typedef struct {
    char ruta[RUTA_MAX];
    uint32_t seq;
    time_t mtime;
} pieza_t;

static TaskHandle_t s_tarea = NULL;
static volatile bool s_activa = false;
static volatile accion_t s_acc = ACC_NINGUNA;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int grupo(const pieza_t *p)
{
    if (p->seq > 0) {
        return 2;
    }
    if (strstr(p->ruta, "/hechos/") != NULL) {
        return 0;
    }
    return 1;
}

static int comparar(const void *a, const void *b)
{
    const pieza_t *pa = a;
    const pieza_t *pb = b;
    int ga = grupo(pa);
    int gb = grupo(pb);
    if (ga != gb) {
        return ga < gb ? -1 : 1;
    }
    if (pa->seq != pb->seq) {
        return pa->seq < pb->seq ? -1 : 1;
    }
    if (pa->mtime != pb->mtime) {
        return pa->mtime < pb->mtime ? -1 : 1;
    }
    return strcmp(pa->ruta, pb->ruta);
}

static bool id_valido(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static bool es_opus(const char *nombre)
{
    size_t n = strlen(nombre);
    return n > 5 && strcasecmp(nombre + n - 5, ".opus") == 0;
}

static bool nombre_es_id(const char *nombre, const char *id)
{
    size_t n = strlen(nombre);
    size_t idn = strlen(id);
    if (n > 5 && strcasecmp(nombre + n - 5, ".opus") == 0) {
        n -= 5;
    }
    if (n == idn && strncmp(nombre, id, idn) == 0) {
        return true;
    }
    return n > idn + 1 && nombre[n - idn - 1] == '_' && strncmp(nombre + (n - idn), id, idn) == 0;
}

static uint32_t seq_de_nombre(const char *nombre)
{
    for (int i = 0; i < 8; i++) {
        if (nombre[i] < '0' || nombre[i] > '9') {
            return 0;
        }
    }
    if (nombre[8] != '_') {
        return 0;
    }
    return (uint32_t)strtoul(nombre, NULL, 10);
}

static bool dir_tiene_id(const char *dir, const char *id)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool si = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (nombre_es_id(e->d_name, id)) {
            si = true;
            break;
        }
    }
    closedir(d);
    return si;
}

static bool ya_esta(const char *id)
{
    return dir_tiene_id(DIR_ESCUCHA, id) || dir_tiene_id(DIR_ESCUCHA_HECHOS, id);
}

static void acusar(const char *id)
{
    char camino[96];
    snprintf(camino, sizeof(camino), "/audios/%s/ack", id);
    int estado = cliente_post(camino);
    ESP_LOGI(TAG, "ACK %s -> %d", id, estado);
}

void escucha_sincronizar(void)
{
    if (!almacen_montada()) {
        return;
    }
    char *man = heap_caps_malloc(TAM_MANIFIESTO, MALLOC_CAP_SPIRAM);
    if (!man) {
        return;
    }
    int estado = cliente_get_texto("/audios/manifest", man, TAM_MANIFIESTO);
    if (estado != 200) {
        ESP_LOGW(TAG, "Manifiesto de audios: HTTP %d", estado);
        free(man);
        return;
    }
    char *p = man;
    int bajados = 0;
    while (*p && bajados < MAX_AUDIOS) {
        char *fin = strpbrk(p, "\r\n");
        if (fin) {
            *fin = '\0';
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char id[80] = "";
        if (sscanf(p, "%79s", id) == 1) {
            size_t n = strlen(id);
            if (n > 5 && strcasecmp(id + n - 5, ".opus") == 0) {
                id[n - 5] = '\0';
            }
            if (id_valido(id)) {
                if (ya_esta(id)) {
                    acusar(id);
                } else {
                    uint32_t seq = estado_leer_u32("escucha_seq", 0) + 1;
                    if (seq == 0) {
                        seq = 1;
                    }
                    char parcial[RUTA_MAX], destino[RUTA_MAX], camino[96];
                    snprintf(parcial, sizeof(parcial), "%s/%08lu.part", DIR_ESCUCHA, (unsigned long)seq);
                    snprintf(destino, sizeof(destino), "%s/%08lu_%s.opus", DIR_ESCUCHA,
                             (unsigned long)seq, id);
                    snprintf(camino, sizeof(camino), "/audios/%s.opus", id);
                    uint8_t sha[32];
                    long bytes = 0;
                    int cod = cliente_descargar(camino, parcial, sha, &bytes);
                    if (cod == 200 && bytes > 0 && rename(parcial, destino) == 0) {
                        estado_guardar_u32("escucha_seq", seq);
                        acusar(id);
                        bajados++;
                        ESP_LOGI(TAG, "Audio %s (%ld bytes)", id, bytes);
                    } else {
                        remove(parcial);
                        ESP_LOGW(TAG, "No se pudo bajar %s (HTTP %d)", id, cod);
                    }
                }
            }
        }
        if (!fin) {
            break;
        }
        p = fin + 1;
    }
    free(man);
}

static void anotar(pieza_t *out, int *n, int max, const char *ruta, const char *nombre, time_t mtime)
{
    uint32_t seq = seq_de_nombre(nombre);
    if (*n < max) {
        snprintf(out[*n].ruta, RUTA_MAX, "%s", ruta);
        out[*n].seq = seq;
        out[*n].mtime = mtime;
        (*n)++;
        return;
    }
    int vieja = 0;
    for (int i = 1; i < *n; i++) {
        if (comparar(&out[i], &out[vieja]) < 0) {
            vieja = i;
        }
    }
    pieza_t nueva = {0};
    snprintf(nueva.ruta, RUTA_MAX, "%s", ruta);
    nueva.seq = seq;
    nueva.mtime = mtime;
    if (comparar(&nueva, &out[vieja]) > 0) {
        out[vieja] = nueva;
    }
}

static int listar_dir(const char *dir, pieza_t *out, int n, int max)
{
    DIR *d = opendir(dir);
    if (!d) {
        return n;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!es_opus(e->d_name)) {
            continue;
        }
        char ruta[RUTA_MAX];
        snprintf(ruta, sizeof(ruta), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(ruta, &st) != 0 || st.st_size <= 0) {
            continue;
        }
        anotar(out, &n, max, ruta, e->d_name, st.st_mtime);
    }
    closedir(d);
    return n;
}

static int listar(pieza_t *out, int max)
{
    int n = listar_dir(DIR_ESCUCHA, out, 0, max);
    n = listar_dir(DIR_ESCUCHA_HECHOS, out, n, max);
    if (n > 1) {
        qsort(out, n, sizeof(pieza_t), comparar);
    }
    return n;
}

static accion_t tomar_accion(void)
{
    accion_t acc;
    taskENTER_CRITICAL(&s_mux);
    acc = s_acc;
    s_acc = ACC_NINGUNA;
    taskEXIT_CRITICAL(&s_mux);
    return acc;
}

static void pedir(accion_t acc)
{
    taskENTER_CRITICAL(&s_mux);
    s_acc = acc;
    taskEXIT_CRITICAL(&s_mux);
    audio_reproducir_parar();
}

static void tarea_reproducir(void *arg)
{
    (void)arg;
    int indice = 0;
    bool arranque = true;
    audio_reproduccion_ocupar(true);
    for (;;) {
        pieza_t *lista = heap_caps_malloc(MAX_AUDIOS * sizeof(pieza_t), MALLOC_CAP_SPIRAM);
        if (!lista) {
            audio_tono(TONO_ERROR);
            break;
        }
        int n = listar(lista, MAX_AUDIOS);
        if (n == 0) {
            free(lista);
            audio_tono(TONO_ERROR);
            break;
        }
        accion_t acc = tomar_accion();
        if (arranque || acc == ACC_ULTIMA) {
            indice = n - 1;
            arranque = false;
        } else if (acc == ACC_ANTERIOR) {
            if (indice > 0) {
                indice--;
            }
        } else if (acc == ACC_SIGUIENTE) {
            if (indice + 1 >= n) {
                free(lista);
                break;
            }
            indice++;
        } else if (acc == ACC_PARAR || acc == ACC_NINGUNA) {
            free(lista);
            break;
        }
        if (indice < 0) {
            indice = 0;
        }
        if (indice >= n) {
            indice = n - 1;
        }
        char ruta[RUTA_MAX];
        memcpy(ruta, lista[indice].ruta, RUTA_MAX);
        free(lista);
        audio_pitidos(1);
        audio_reproduccion_ocupar(true);
        bool ok = audio_reproducir_opus(ruta);
        audio_reproduccion_ocupar(true);
        if (!ok && s_acc == ACC_NINGUNA) {
            audio_tono(TONO_ERROR);
            break;
        }
    }
    audio_reproduccion_ocupar(false);
    s_activa = false;
    s_tarea = NULL;
    vTaskDelete(NULL);
}

static void lanzar(void)
{
    if (s_tarea) {
        return;
    }
    s_activa = true;
    if (xTaskCreate(tarea_reproducir, "escucha", 40960, NULL, 5, &s_tarea) != pdPASS) {
        s_tarea = NULL;
        s_activa = false;
        audio_tono(TONO_ERROR);
    }
}

bool escucha_activa(void)
{
    return s_activa;
}

void escucha_empezar(void)
{
    if (audio_grabando() || !almacen_montada()) {
        return;
    }
    if (s_activa) {
        pedir(ACC_ULTIMA);
        return;
    }
    lanzar();
}

void escucha_anterior(void)
{
    if (!s_activa) {
        return;
    }
    pedir(ACC_ANTERIOR);
}

void escucha_siguiente(void)
{
    if (!s_activa) {
        return;
    }
    pedir(ACC_SIGUIENTE);
}

void escucha_parar(void)
{
    if (!s_activa) {
        return;
    }
    pedir(ACC_PARAR);
}
