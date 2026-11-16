#include "red.h"

#include <string.h>
#include <time.h>

#include "ajustes.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "reloj.h"

static const char *TAG = "red";

#define BIT_IP BIT0
#define BIT_FALLO BIT1
#define TIMEOUT_CONEXION_MS 15000
#define MAX_AP 20

static EventGroupHandle_t s_eventos = NULL;
static bool s_iniciado = false;
static bool s_encendido = false;

static void manejador(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eventos, BIT_IP);
        xEventGroupSetBits(s_eventos, BIT_FALLO);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_eventos, BIT_IP);
    }
}

static bool iniciar(void)
{
    if (s_iniciado) {
        return true;
    }
    s_eventos = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, manejador, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, manejador, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_iniciado = true;
    return true;
}

static bool conectar_a(const red_wifi_t *red)
{
    wifi_config_t wc = {0};
    // Los campos no necesitan '\0' final: un SSID puede ocupar los 32 bytes.
    memcpy(wc.sta.ssid, red->ssid, strnlen(red->ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, red->pass, strnlen(red->pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = red->pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_eventos, BIT_IP | BIT_FALLO);
    ESP_LOGI(TAG, "Conectando a %s", red->ssid);
    if (esp_wifi_connect() != ESP_OK) {
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(s_eventos, BIT_IP | BIT_FALLO, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(TIMEOUT_CONEXION_MS));
    if (b & BIT_IP) {
        return true;
    }
    esp_wifi_disconnect();
    // Que el evento de esta desconexión no aborte el intento con la siguiente red.
    vTaskDelay(pdMS_TO_TICKS(300));
    return false;
}

// En memoria RTC: sobrevive al sueño profundo.
RTC_DATA_ATTR static time_t s_ultima_sntp = 0;

static void sincronizar_hora(void)
{
    time_t ultima = s_ultima_sntp;
    time_t ahora = time(NULL);
    if (reloj_valido() && ultima && ahora - ultima < 24 * 3600) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        s_ultima_sntp = time(NULL);
        reloj_guardar_rtc();
    } else {
        ESP_LOGW(TAG, "SNTP sin respuesta");
    }
    esp_netif_sntp_deinit();
}

bool red_conectar(void)
{
    if (g_ajustes.n_wifi == 0) {
        ESP_LOGW(TAG, "No hay redes Wi-Fi configuradas");
        return false;
    }
    ESP_LOGI(TAG, "Iniciando Wi-Fi (%d redes)", g_ajustes.n_wifi);
    if (!iniciar()) {
        ESP_LOGE(TAG, "Fallo al iniciar la pila Wi-Fi");
        return false;
    }
    if (s_encendido && (xEventGroupGetBits(s_eventos) & BIT_IP)) {
        return true;
    }
    if (!s_encendido) {
        if (esp_wifi_start() != ESP_OK) {
            return false;
        }
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_encendido = true;
    }

    // Orden de prueba: redes configuradas visibles, de más a menos señal; después el resto
    // (pueden ser redes ocultas que el escaneo no muestra).
    int orden[AJUSTES_MAX_WIFI];
    int8_t rssi[AJUSTES_MAX_WIFI];
    int n = 0;
    for (int i = 0; i < g_ajustes.n_wifi; i++) {
        rssi[i] = -128;
    }
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t num = MAX_AP;
        wifi_ap_record_t aps[MAX_AP];
        if (esp_wifi_scan_get_ap_records(&num, aps) == ESP_OK) {
            for (int a = 0; a < num; a++) {
                for (int i = 0; i < g_ajustes.n_wifi; i++) {
                    if (strcmp((const char *)aps[a].ssid, g_ajustes.wifi[i].ssid) == 0 && aps[a].rssi > rssi[i]) {
                        rssi[i] = aps[a].rssi;
                    }
                }
            }
        }
    }
    for (int i = 0; i < g_ajustes.n_wifi; i++) {
        orden[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && rssi[orden[j]] > rssi[orden[j - 1]]; j--) {
            int t = orden[j];
            orden[j] = orden[j - 1];
            orden[j - 1] = t;
        }
    }
    for (int k = 0; k < n; k++) {
        if (conectar_a(&g_ajustes.wifi[orden[k]])) {
            ESP_LOGI(TAG, "Conectado a %s", g_ajustes.wifi[orden[k]].ssid);
            sincronizar_hora();
            return true;
        }
    }
    ESP_LOGW(TAG, "No se pudo conectar a ninguna red");
    red_desconectar();
    return false;
}

void red_desconectar(void)
{
    if (s_encendido) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_encendido = false;
    }
}
