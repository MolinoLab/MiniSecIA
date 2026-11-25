#include "cliente.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ajustes.h"
#include "almacen.h"
#include "audio.h"
#include "energia.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "cliente";

#define LIMITE "----minisecia7d3f9a1c40b2"
#define TROZO (8 * 1024)

typedef struct {
    char estado[16];
    char audio_flag[4];
    char audio_path[96];
    char meditacion[4];
    char meditacion_min[8];
} respuesta_t;

static esp_err_t manejador(esp_http_client_event_t *evt)
{
    respuesta_t *r = (respuesta_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && r && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "X-Voz-Estado") == 0) {
            strlcpy(r->estado, evt->header_value, sizeof(r->estado));
        } else if (strcasecmp(evt->header_key, "X-Voz-Audio") == 0) {
            strlcpy(r->audio_flag, evt->header_value, sizeof(r->audio_flag));
        } else if (strcasecmp(evt->header_key, "X-Voz-Audio-Path") == 0) {
            strlcpy(r->audio_path, evt->header_value, sizeof(r->audio_path));
        } else if (strcasecmp(evt->header_key, "X-Voz-Meditacion") == 0) {
            strlcpy(r->meditacion, evt->header_value, sizeof(r->meditacion));
        } else if (strcasecmp(evt->header_key, "X-Voz-Meditacion-Min") == 0) {
            strlcpy(r->meditacion_min, evt->header_value, sizeof(r->meditacion_min));
        }
    }
    return ESP_OK;
}

static esp_http_client_handle_t crear(const char *camino, esp_http_client_method_t metodo, respuesta_t *r)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", g_ajustes.url, camino);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = metodo,
        .timeout_ms = g_ajustes.timeout_http_seg * 1000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = manejador,
        .user_data = r,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", g_ajustes.token);
    esp_http_client_set_header(c, "Authorization", auth);
    return c;
}

static bool escribir_todo(esp_http_client_handle_t c, const char *datos, int len)
{
    while (len > 0) {
        int n = esp_http_client_write(c, datos, len);
        if (n <= 0) {
            return false;
        }
        datos += n;
        len -= n;
    }
    return true;
}

static int leer_cuerpo(esp_http_client_handle_t c, char *buf, size_t n)
{
    size_t total = 0;
    if (n == 0) {
        return 0;
    }
    for (;;) {
        int k = esp_http_client_read(c, buf + total, (int)(n - 1 - total));
        if (k <= 0) {
            break;
        }
        total += k;
        if (total >= n - 1) {
            break;
        }
    }
    buf[total] = '\0';
    return (int)total;
}

resultado_subida_t cliente_subir(const char *ruta, const char *request_id, const char *grabado_en,
                                 char *audio_path, size_t audio_path_n, int *meditacion_min)
{
    if (audio_path && audio_path_n > 0) {
        audio_path[0] = '\0';
    }
    if (meditacion_min) {
        *meditacion_min = 0;
    }
    long tam = almacen_tamano(ruta);
    FILE *f = tam > 0 ? fopen(ruta, "rb") : NULL;
    if (!f) {
        ESP_LOGE(TAG, "No se puede leer %s", ruta);
        return SUBIDA_RECHAZADA;
    }
    const char *nombre = strrchr(ruta, '/');
    nombre = nombre ? nombre + 1 : ruta;

    char cabeza[640];
    int n = snprintf(cabeza, sizeof(cabeza),
                     "--" LIMITE "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n%s\r\n",
                     g_ajustes.device_id);
    if (grabado_en && grabado_en[0]) {
        n += snprintf(cabeza + n, sizeof(cabeza) - n,
                      "--" LIMITE "\r\nContent-Disposition: form-data; name=\"grabado_en\"\r\n\r\n%s\r\n",
                      grabado_en);
    }
    n += snprintf(cabeza + n, sizeof(cabeza) - n,
                  "--" LIMITE "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"%s\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n",
                  nombre);
    static const char COLA[] = "\r\n--" LIMITE "--\r\n";
    int total = n + (int)tam + (int)(sizeof(COLA) - 1);

    respuesta_t resp = {0};
    esp_http_client_handle_t c = crear("/voz", HTTP_METHOD_POST, &resp);
    char *trozo = heap_caps_malloc(TROZO, MALLOC_CAP_SPIRAM);
    if (!c || !trozo) {
        fclose(f);
        free(trozo);
        if (c) {
            esp_http_client_cleanup(c);
        }
        return SUBIDA_REINTENTAR;
    }
    char bateria[12];
    snprintf(bateria, sizeof(bateria), "%d", energia_bateria_mv());
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" LIMITE);
    esp_http_client_set_header(c, "X-Request-Id", request_id);
    esp_http_client_set_header(c, "X-Bateria-mV", bateria);

    resultado_subida_t res = SUBIDA_REINTENTAR;
    int estado = -1;
    bool enviado = false;
    if (esp_http_client_open(c, total) == ESP_OK && escribir_todo(c, cabeza, n)) {
        enviado = true;
        size_t k;
        while ((k = fread(trozo, 1, TROZO, f)) > 0) {
            if (!escribir_todo(c, trozo, (int)k)) {
                enviado = false;
                break;
            }
        }
        enviado = enviado && escribir_todo(c, COLA, sizeof(COLA) - 1);
    }
    fclose(f);

    if (enviado && esp_http_client_fetch_headers(c) >= 0) {
        estado = esp_http_client_get_status_code(c);
        leer_cuerpo(c, trozo, 512);
    } else {
        trozo[0] = '\0';
    }
    ESP_LOGI(TAG, "POST /voz %s -> %d %s audio=%s path=%s %s", request_id, estado, resp.estado,
             resp.audio_flag, resp.audio_path, trozo);

    if (estado == 200) {
        if (strcmp(resp.estado, "pendiente") == 0) {
            res = SUBIDA_PENDIENTE;
        } else if (strcmp(resp.estado, "meditacion") == 0 || resp.meditacion[0] == '1') {
            res = SUBIDA_MEDITACION;
            if (meditacion_min) {
                int m = atoi(resp.meditacion_min);
                if (m < 1) {
                    m = 10;
                }
                if (m > 120) {
                    m = 120;
                }
                *meditacion_min = m;
            }
        } else {
            res = SUBIDA_OK;
            bool hay_audio = (strcmp(resp.estado, "audio") == 0) || (resp.audio_flag[0] == '1');
            if (hay_audio && resp.audio_path[0] == '/' && audio_path && audio_path_n > 1) {
                strlcpy(audio_path, resp.audio_path, audio_path_n);
            }
        }
    } else if (estado == 422) {
        // Sin la cabecera es un error de validación de FastAPI (petición mal formada).
        res = strcmp(resp.estado, "vacio") == 0 ? SUBIDA_VACIA : SUBIDA_ERROR_CONFIG;
    } else if (estado == 413) {
        res = SUBIDA_RECHAZADA;
    } else if (estado == 400 || estado == 401 || estado == 403) {
        res = SUBIDA_ERROR_CONFIG;
    }
    free(trozo);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

#define RUTA_TMP_RESPUESTA "/sdcard/tmp_respuesta.opus"

bool cliente_reproducir_respuesta(const char *audio_path)
{
    if (!audio_path || audio_path[0] != '/') {
        return false;
    }
    // Solo paths del servidor de respuestas efímeras.
    static const char PREF[] = "/voz/respuesta/";
    if (strncmp(audio_path, PREF, sizeof(PREF) - 1) != 0) {
        ESP_LOGW(TAG, "Path de audio no permitido: %s", audio_path);
        return false;
    }
    const char *nombre = audio_path + (sizeof(PREF) - 1);
    size_t nlen = strlen(nombre);
    if (nlen < 6 || strcmp(nombre + nlen - 5, ".opus") != 0) {
        return false;
    }
    char id[80];
    if (nlen - 5 >= sizeof(id)) {
        return false;
    }
    memcpy(id, nombre, nlen - 5);
    id[nlen - 5] = '\0';

    uint8_t sha[32];
    long bytes = 0;
    remove(RUTA_TMP_RESPUESTA);
    int cod = cliente_descargar(audio_path, RUTA_TMP_RESPUESTA, sha, &bytes);
    if (cod != 200 || bytes <= 0) {
        ESP_LOGW(TAG, "No se pudo bajar respuesta %s (HTTP %d)", audio_path, cod);
        remove(RUTA_TMP_RESPUESTA);
        return false;
    }
    ESP_LOGI(TAG, "Reproduciendo respuesta %s (%ld bytes)", id, bytes);
    bool ok = audio_reproducir_opus(RUTA_TMP_RESPUESTA);
    remove(RUTA_TMP_RESPUESTA);

    char ack[128];
    snprintf(ack, sizeof(ack), "/voz/respuesta/%s/ack", id);
    int ack_cod = cliente_post(ack);
    ESP_LOGI(TAG, "ACK respuesta %s -> %d", id, ack_cod);
    return ok;
}

int cliente_get_texto(const char *camino, char *buf, size_t n)
{
    esp_http_client_handle_t c = crear(camino, HTTP_METHOD_GET, NULL);
    int estado = -1;
    if (n) {
        buf[0] = '\0';
    }
    if (!c) {
        return -1;
    }
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        estado = esp_http_client_get_status_code(c);
        leer_cuerpo(c, buf, n);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return estado;
}

int cliente_descargar(const char *camino, const char *destino, uint8_t sha256[32], long *bytes)
{
    esp_http_client_handle_t c = crear(camino, HTTP_METHOD_GET, NULL);
    if (!c) {
        return -1;
    }
    int estado = -1;
    long total = 0;
    char *trozo = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    FILE *f = NULL;
    if (trozo && esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        estado = esp_http_client_get_status_code(c);
        if (estado == 200 && (f = fopen(destino, "wb")) != NULL) {
            mbedtls_sha256_context ctx;
            mbedtls_sha256_init(&ctx);
            mbedtls_sha256_starts(&ctx, 0);
            int k;
            while ((k = esp_http_client_read(c, trozo, 4096)) > 0) {
                if (fwrite(trozo, 1, k, f) != (size_t)k) {
                    estado = -1;
                    break;
                }
                mbedtls_sha256_update(&ctx, (const unsigned char *)trozo, k);
                total += k;
            }
            if (k < 0) {
                estado = -1;
            }
            mbedtls_sha256_finish(&ctx, sha256);
            mbedtls_sha256_free(&ctx);
            fclose(f);
        }
    }
    free(trozo);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (bytes) {
        *bytes = total;
    }
    return estado;
}

int cliente_post(const char *camino)
{
    esp_http_client_handle_t c = crear(camino, HTTP_METHOD_POST, NULL);
    if (!c) {
        return -1;
    }
    int estado = -1;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        estado = esp_http_client_get_status_code(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return estado;
}
