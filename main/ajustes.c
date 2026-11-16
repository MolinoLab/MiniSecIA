#include "ajustes.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "placa.h"
#include "sdkconfig.h"

static const char *TAG = "ajustes";
static const char *NS_AJUSTES = "ajustes";
static const char *NS_ESTADO = "estado";
static const char *RUTA_CONFIG = PUNTO_MONTAJE "/config.txt";

ajustes_t g_ajustes;

static void copiar(char *dst, size_t n, const char *src)
{
    strlcpy(dst, src ? src : "", n);
}

static void valores_por_defecto(ajustes_t *a)
{
    memset(a, 0, sizeof(*a));
    copiar(a->wifi[0].ssid, sizeof(a->wifi[0].ssid), CONFIG_MS_WIFI1_SSID);
    copiar(a->wifi[0].pass, sizeof(a->wifi[0].pass), CONFIG_MS_WIFI1_PASS);
    copiar(a->wifi[1].ssid, sizeof(a->wifi[1].ssid), CONFIG_MS_WIFI2_SSID);
    copiar(a->wifi[1].pass, sizeof(a->wifi[1].pass), CONFIG_MS_WIFI2_PASS);
    copiar(a->url, sizeof(a->url), CONFIG_MS_URL_SERVIDOR);
    copiar(a->token, sizeof(a->token), CONFIG_MS_TOKEN);
    copiar(a->device_id, sizeof(a->device_id), CONFIG_MS_DEVICE_ID);
    a->volumen = CONFIG_MS_VOLUMEN;
    a->ganancia_mic = CONFIG_MS_GANANCIA_MIC;
    a->max_grabacion_seg = CONFIG_MS_MAX_GRABACION_SEG;
    a->reposo_seg = CONFIG_MS_REPOSO_SEG;
    a->reintento_min = CONFIG_MS_REINTENTO_MIN;
    a->imagen_horas = CONFIG_MS_IMAGEN_HORAS;
    a->sync_imagenes_horas = CONFIG_MS_SYNC_IMAGENES_HORAS;
    a->timeout_http_seg = CONFIG_MS_TIMEOUT_HTTP_SEG;
}

typedef enum { T_STR, T_INT } tipo_t;

typedef struct {
    const char *clave;  // en config.txt
    const char *nvs;    // en NVS (máximo 15 caracteres)
    tipo_t tipo;
    size_t offset;
    size_t tam;
    int min, max;
} campo_t;

#define CAMPO_STR(k, n, f) {k, n, T_STR, offsetof(ajustes_t, f), sizeof(((ajustes_t *)0)->f), 0, 0}
#define CAMPO_INT(k, n, f, lo, hi) {k, n, T_INT, offsetof(ajustes_t, f), sizeof(int), lo, hi}

static const campo_t CAMPOS[] = {
    CAMPO_STR("wifi1_ssid", "w1s", wifi[0].ssid), CAMPO_STR("wifi1_pass", "w1p", wifi[0].pass),
    CAMPO_STR("wifi2_ssid", "w2s", wifi[1].ssid), CAMPO_STR("wifi2_pass", "w2p", wifi[1].pass),
    CAMPO_STR("wifi3_ssid", "w3s", wifi[2].ssid), CAMPO_STR("wifi3_pass", "w3p", wifi[2].pass),
    CAMPO_STR("url", "url", url),
    CAMPO_STR("token", "token", token),
    CAMPO_STR("device_id", "device_id", device_id),
    CAMPO_INT("volumen", "volumen", volumen, 0, 100),
    CAMPO_INT("ganancia_mic", "ganancia", ganancia_mic, 0, 42),
    CAMPO_INT("max_grabacion_seg", "max_grab", max_grabacion_seg, 10, 420),
    CAMPO_INT("reposo_seg", "reposo", reposo_seg, 20, 3600),
    CAMPO_INT("reintento_min", "reintento", reintento_min, 1, 1440),
    CAMPO_INT("imagen_horas", "img_horas", imagen_horas, 0, 720),
    CAMPO_INT("sync_imagenes_horas", "sync_img_h", sync_imagenes_horas, 1, 720),
    CAMPO_INT("timeout_http_seg", "timeout_http", timeout_http_seg, 30, 900),
};
#define N_CAMPOS (sizeof(CAMPOS) / sizeof(CAMPOS[0]))

static const campo_t *buscar_campo(const char *clave)
{
    for (size_t i = 0; i < N_CAMPOS; i++) {
        if (strcmp(CAMPOS[i].clave, clave) == 0) {
            return &CAMPOS[i];
        }
    }
    return NULL;
}

static bool asignar(ajustes_t *a, const campo_t *c, const char *valor)
{
    char *dst = (char *)a + c->offset;
    if (c->tipo == T_STR) {
        if (strlen(valor) >= c->tam) {
            ESP_LOGW(TAG, "%s demasiado largo (max %u)", c->clave, (unsigned)(c->tam - 1));
            return false;
        }
        copiar(dst, c->tam, valor);
        return true;
    }
    char *fin = NULL;
    long v = strtol(valor, &fin, 10);
    // Comentario al final solo en números: en textos '#' puede ser parte de una contraseña.
    char *resto = fin;
    while (isspace((unsigned char)*resto)) {
        resto++;
    }
    if (fin == valor || (*resto != '\0' && *resto != '#') || v < c->min || v > c->max) {
        ESP_LOGW(TAG, "%s fuera de rango (%d-%d): %s", c->clave, c->min, c->max, valor);
        return false;
    }
    *(int *)dst = (int)v;
    return true;
}

static void leer_nvs(ajustes_t *a)
{
    nvs_handle_t h;
    if (nvs_open(NS_AJUSTES, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < N_CAMPOS; i++) {
        const campo_t *c = &CAMPOS[i];
        char *dst = (char *)a + c->offset;
        if (c->tipo == T_STR) {
            size_t n = c->tam;
            char tmp[128];
            if (n > sizeof(tmp)) {
                n = sizeof(tmp);
            }
            if (nvs_get_str(h, c->nvs, tmp, &n) == ESP_OK) {
                copiar(dst, c->tam, tmp);
            }
        } else {
            int32_t v;
            if (nvs_get_i32(h, c->nvs, &v) == ESP_OK && v >= c->min && v <= c->max) {
                *(int *)dst = (int)v;
            }
        }
    }
    nvs_close(h);
}

static void guardar_nvs(const ajustes_t *a)
{
    nvs_handle_t h;
    if (nvs_open(NS_AJUSTES, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "No se puede abrir NVS");
        return;
    }
    for (size_t i = 0; i < N_CAMPOS; i++) {
        const campo_t *c = &CAMPOS[i];
        const char *src = (const char *)a + c->offset;
        if (c->tipo == T_STR) {
            nvs_set_str(h, c->nvs, src);
        } else {
            nvs_set_i32(h, c->nvs, *(const int *)src);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static char *recortar(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *fin = s + strlen(s);
    while (fin > s && isspace((unsigned char)fin[-1])) {
        *--fin = '\0';
    }
    return s;
}

// Formato: clave=valor por línea; '#' inicia comentario. Solo se cambian las claves presentes.
static bool importar_config_txt(ajustes_t *a)
{
    FILE *f = fopen(RUTA_CONFIG, "r");
    if (!f) {
        return false;
    }
    char linea[256];
    int aplicadas = 0;
    bool primera = true;
    while (fgets(linea, sizeof(linea), f)) {
        char *l = linea;
        if (primera && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) {
            l += 3;  // BOM UTF-8 (Bloc de notas de Windows)
        }
        primera = false;
        l = recortar(l);
        if (*l == '\0' || *l == '#') {
            continue;
        }
        char *igual = strchr(l, '=');
        if (!igual) {
            continue;
        }
        *igual = '\0';
        char *clave = recortar(l);
        char *valor = recortar(igual + 1);
        for (char *p = clave; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        const campo_t *c = buscar_campo(clave);
        if (!c) {
            ESP_LOGW(TAG, "config.txt: clave desconocida '%s'", clave);
            continue;
        }
        if (asignar(a, c, valor)) {
            aplicadas++;
        }
    }
    fclose(f);
    guardar_nvs(a);
    // Contiene el token y las contraseñas Wi-Fi: no se deja en la tarjeta.
    if (remove(RUTA_CONFIG) != 0) {
        ESP_LOGW(TAG, "No se pudo borrar config.txt");
    }
    ESP_LOGI(TAG, "config.txt importado (%d claves) y borrado", aplicadas);
    return true;
}

static void contar_wifi(ajustes_t *a)
{
    // Compacta las redes configuradas al principio del array.
    red_wifi_t redes[AJUSTES_MAX_WIFI];
    int n = 0;
    for (int i = 0; i < AJUSTES_MAX_WIFI; i++) {
        if (a->wifi[i].ssid[0]) {
            redes[n++] = a->wifi[i];
        }
    }
    memset(a->wifi, 0, sizeof(a->wifi));
    memcpy(a->wifi, redes, sizeof(red_wifi_t) * n);
    a->n_wifi = n;
}

void ajustes_cargar(bool sd_montada)
{
    valores_por_defecto(&g_ajustes);
    leer_nvs(&g_ajustes);
    if (sd_montada) {
        importar_config_txt(&g_ajustes);
    }
    size_t n = strlen(g_ajustes.url);
    while (n > 0 && g_ajustes.url[n - 1] == '/') {
        g_ajustes.url[--n] = '\0';
    }
    // Va en X-Request-Id: el servidor solo acepta [A-Za-z0-9._:-].
    for (char *p = g_ajustes.device_id; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            *p = '-';
        }
    }
    if (!g_ajustes.device_id[0]) {
        strlcpy(g_ajustes.device_id, "esp32", sizeof(g_ajustes.device_id));
    }
    contar_wifi(&g_ajustes);
    ESP_LOGI(TAG, "device_id=%s url=%s redes=%d token=%s", g_ajustes.device_id, g_ajustes.url,
             g_ajustes.n_wifi, g_ajustes.token[0] ? "si" : "NO");
}

bool ajustes_servidor_configurado(void)
{
    return g_ajustes.url[0] && g_ajustes.token[0] && g_ajustes.n_wifi > 0;
}

uint32_t estado_leer_u32(const char *clave, uint32_t defecto)
{
    nvs_handle_t h;
    uint32_t v = defecto;
    if (nvs_open(NS_ESTADO, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u32(h, clave, &v) != ESP_OK) {
            v = defecto;
        }
        nvs_close(h);
    }
    return v;
}

void estado_guardar_u32(const char *clave, uint32_t valor)
{
    nvs_handle_t h;
    if (nvs_open(NS_ESTADO, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, clave, valor);
        nvs_commit(h);
        nvs_close(h);
    }
}
