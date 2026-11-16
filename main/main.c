// MiniSecIA: grabadora de bolsillo que envía notas de voz a ConvocAUTOrias (POST /voz).
//
// BOOT corto: grabar / terminar        PWR corto: cancelar la grabación, o estado de la cola
// BOOT largo: sincronizar e imagen      PWR largo: apagar
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ajustes.h"
#include "almacen.h"
#include "audio.h"
#include "botones.h"
#include "cliente.h"
#include "driver/i2c_master.h"
#include "energia.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imagenes.h"
#include "nvs_flash.h"
#include "pantalla.h"
#include "placa.h"
#include "red.h"
#include "reloj.h"

static const char *TAG = "main";

#define SYNC_AVISAR   (1 << 0)  // iniciada por el usuario: sonido con el resultado
#define SYNC_IMAGENES (1 << 1)  // sincronizar imágenes aunque no toque, y cambiar la imagen
#define SYNC_REVISAR  (1 << 2)  // despertar la tarea de sync (cola / imágenes si toca); no usar 0 con eSetBits
#define LOTE_COLA 16
#define BYTES_MIN_GRABACION (AUDIO_HZ * 2 / 2)  // 0,5 s
#define BATERIA_BAJA_MV 3450
#define REPOSO_RAPIDO_SEG 5

// Fallos de subida seguidos (sin red o servidor caído): alargan la espera entre reintentos
// durante el sueño para no gastar batería. En memoria RTC, sobrevive al sueño profundo.
RTC_DATA_ATTR static uint32_t s_fallos_seguidos = 0;
#define REINTENTO_MAX_SEG (6 * 3600)

static i2c_master_bus_handle_t s_i2c = NULL;
static TaskHandle_t s_tarea_sync = NULL;
static volatile bool s_sync_en_curso = false;
static volatile bool s_no_autorizado = false;
static volatile int64_t s_actividad_us = 0;
static volatile int64_t s_ultimo_intento_us = 0;
static bool s_reposo_rapido = false;
static bool s_audio_ok = false;
static uint32_t s_seq = 0;
static uint32_t s_instancia = 0;
static char s_grabacion[RUTA_MAX];

static int64_t ahora_us(void)
{
    return esp_timer_get_time();
}

static void actividad(void)
{
    s_actividad_us = ahora_us();
}

static void pedir_sync(uint32_t bits)
{
    if (s_tarea_sync) {
        // eSetBits con 0 no aporta bits útiles; aseguramos al menos SYNC_REVISAR.
        xTaskNotify(s_tarea_sync, bits ? bits : SYNC_REVISAR, eSetBits);
    }
}

// --- Cola de subida ---

static void request_id(const char *ruta, char *rid, size_t n, char *iso, size_t n_iso)
{
    const char *nombre = strrchr(ruta, '/');
    nombre = nombre ? nombre + 1 : ruta;
    unsigned long seq = strtoul(nombre, NULL, 10);
    // Id único por audio y estable entre reintentos: el servidor no lo procesa dos veces.
    snprintf(rid, n, "%s-%08lx-%lu", g_ajustes.device_id, (unsigned long)s_instancia, seq);
    char ts[20] = "";
    const char *guion = strchr(nombre, '_');
    if (guion) {
        strlcpy(ts, guion + 1, sizeof(ts));
        char *punto = strchr(ts, '.');
        if (punto) {
            *punto = '\0';
        }
    }
    reloj_compacto_a_iso(ts, iso, n_iso);
}

// Devuelve el resultado del último audio procesado (o -1 si no había ninguno).
static int subir_cola(void)
{
    static char rutas[LOTE_COLA][RUTA_MAX];
    int ultimo = -1;
    for (;;) {
        int total = 0;
        int n = almacen_listar_cola(rutas, LOTE_COLA, &total);
        if (n == 0) {
            return ultimo;
        }
        for (int i = 0; i < n; i++) {
            char rid[80], iso[24];
            request_id(rutas[i], rid, sizeof(rid), iso, sizeof(iso));
            resultado_subida_t r = cliente_subir(rutas[i], rid, iso);
            ultimo = r;
            actividad();
            switch (r) {
            case SUBIDA_OK:
            case SUBIDA_PENDIENTE:
            case SUBIDA_VACIA:
                remove(rutas[i]);
                break;
            case SUBIDA_RECHAZADA:
                almacen_mover_a_fallidos(rutas[i]);
                break;
            case SUBIDA_ERROR_CONFIG:
                ESP_LOGE(TAG, "El servidor rechaza token o device_id: se detiene la cola hasta reiniciar");
                s_no_autorizado = true;
                return ultimo;
            case SUBIDA_REINTENTAR:
                return ultimo;
            }
        }
        if (total <= n) {
            return ultimo;
        }
    }
}

static void tono_resultado(int r)
{
    switch (r) {
    case SUBIDA_OK:
        audio_tono(TONO_OK);
        break;
    case SUBIDA_PENDIENTE:
        audio_tono(TONO_PENDIENTE);
        break;
    case SUBIDA_REINTENTAR:
        audio_tono(TONO_EN_COLA);
        break;
    case SUBIDA_VACIA:
    case SUBIDA_RECHAZADA:
    case SUBIDA_ERROR_CONFIG:
        audio_tono(TONO_ERROR);
        break;
    default:
        break;
    }
}

static void tarea_sync(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        bool avisar = bits & SYNC_AVISAR;
        bool forzar_img = bits & SYNC_IMAGENES;
        int en_cola = almacen_montada() ? almacen_contar_cola() : 0;
        bool img = almacen_montada() && (forzar_img || imagenes_toca_sincronizar());
        ESP_LOGI(TAG, "Sync bits=0x%lx cola=%d img=%d", (unsigned long)bits, en_cola, img ? 1 : 0);
        if (en_cola == 0 && !img) {
            continue;
        }
        s_sync_en_curso = true;
        s_ultimo_intento_us = ahora_us();
        actividad();

        if (!ajustes_servidor_configurado() || s_no_autorizado) {
            ESP_LOGW(TAG, "Servidor o Wi-Fi sin configurar (o token rechazado)");
            if (avisar) {
                audio_tono(TONO_ERROR);
            }
        } else {
            if (!audio_grabando()) {
                energia_led(LED_PARPADEO);
            }
            if (!red_conectar()) {
                s_fallos_seguidos++;
                if (avisar && en_cola) {
                    audio_tono(TONO_EN_COLA);
                }
            } else {
                int r = en_cola ? subir_cola() : -1;
                if (r == SUBIDA_REINTENTAR) {
                    s_fallos_seguidos++;
                } else {
                    s_fallos_seguidos = 0;
                }
                if (avisar) {
                    tono_resultado(r);
                }
                if (img) {
                    imagenes_sincronizar(forzar_img);
                }
                red_desconectar();
            }
            if (!audio_grabando()) {
                energia_led(LED_APAGADO);
            }
        }
        if (forzar_img) {
            imagenes_siguiente();
        }
        s_sync_en_curso = false;
        actividad();
    }
}

// --- Grabación ---

static void empezar_grabacion(void)
{
    if (!almacen_montada() || !s_audio_ok) {
        audio_tono(TONO_ERROR);
        return;
    }
    audio_tono(TONO_INICIO);
    s_seq++;
    estado_guardar_u32("seq", s_seq);
    char ts[20];
    reloj_compacto(time(NULL), ts, sizeof(ts));
    snprintf(s_grabacion, sizeof(s_grabacion), "%s/%08lu_%s.tmp", DIR_COLA, (unsigned long)s_seq, ts);
    if (!audio_grabar_iniciar(s_grabacion, g_ajustes.max_grabacion_seg)) {
        audio_tono(TONO_ERROR);
        return;
    }
    energia_led(LED_FIJO);
}

static void terminar_grabacion(bool cancelar)
{
    uint32_t bytes = audio_grabar_parar(cancelar);
    energia_led(s_sync_en_curso ? LED_PARPADEO : LED_APAGADO);
    if (cancelar || bytes < BYTES_MIN_GRABACION) {
        remove(s_grabacion);
        audio_tono(TONO_CANCELADO);
        return;
    }
    char wav[RUTA_MAX];
    strlcpy(wav, s_grabacion, sizeof(wav));
    memcpy(wav + strlen(wav) - 4, ".wav", 4);
    if (rename(s_grabacion, wav) != 0) {
        ESP_LOGE(TAG, "No se pudo renombrar %s", s_grabacion);
        audio_tono(TONO_ERROR);
        return;
    }
    audio_tono(TONO_FIN);
    pedir_sync(SYNC_AVISAR);
}

// --- Energía ---

static void avisar_bateria(void)
{
    int mv = energia_bateria_mv();
    ESP_LOGI(TAG, "Batería: %d mV", mv);
    if (mv > 1000 && mv < BATERIA_BAJA_MV) {
        audio_tono(TONO_BATERIA_BAJA);
    }
}

static void esperar_pantalla(void)
{
    for (int i = 0; i < 600 && pantalla_ocupada(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void apagar(void)
{
    if (audio_grabando()) {
        terminar_grabacion(false);  // se queda en la cola para la próxima vez
    }
    audio_tono(TONO_APAGADO);
    esperar_pantalla();
    red_desconectar();
    audio_apagar();
    almacen_desmontar();
    energia_apagar();
}

static void dormir(void)
{
    uint32_t seg = 0;
    if (almacen_montada() && almacen_contar_cola() > 0 && ajustes_servidor_configurado() && !s_no_autorizado) {
        uint32_t factor = 1u << (s_fallos_seguidos < 5 ? s_fallos_seguidos : 5);
        seg = (uint32_t)g_ajustes.reintento_min * 60 * factor;
        if (seg > REINTENTO_MAX_SEG) {
            seg = REINTENTO_MAX_SEG;
        }
    }
    uint32_t rot = imagenes_segundos_hasta_rotacion();
    if (rot && (seg == 0 || rot < seg)) {
        seg = rot;
    }
    red_desconectar();
    audio_apagar();
    almacen_desmontar();
    energia_dormir(seg);
}

static void manejar_boton(evento_boton_t ev)
{
    s_reposo_rapido = false;
    switch (ev) {
    case EV_BOOT_CORTO:
        if (audio_grabando()) {
            terminar_grabacion(false);
        } else {
            empezar_grabacion();
        }
        break;
    case EV_PWR_CORTO:
        if (audio_grabando()) {
            terminar_grabacion(true);
        } else {
            int n = almacen_montada() ? almacen_contar_cola() : 0;
            if (n == 0) {
                audio_tono(TONO_OK);
            } else {
                audio_pitidos(n);
                pedir_sync(SYNC_AVISAR);
            }
            avisar_bateria();
        }
        break;
    case EV_BOOT_LARGO:
        if (!audio_grabando()) {
            audio_pitidos(1);
            pedir_sync(SYNC_AVISAR | SYNC_IMAGENES);
        }
        break;
    case EV_PWR_LARGO:
        apagar();
        break;
    }
}

static void iniciar_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
}

void app_main(void)
{
    despertar_t causa = energia_iniciar();
    ESP_LOGI(TAG, "Arranque (causa %d)", causa);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    iniciar_i2c();
    reloj_iniciar(s_i2c);
    botones_iniciar(causa == DESPERTAR_BOOT);

    bool sd = almacen_montar();
    ajustes_cargar(sd);
    s_audio_ok = audio_iniciar(s_i2c, g_ajustes.volumen, g_ajustes.ganancia_mic);
    pantalla_iniciar();

    s_instancia = estado_leer_u32("instancia", 0);
    if (s_instancia == 0) {
        s_instancia = esp_random() | 1;
        estado_guardar_u32("instancia", s_instancia);
    }
    s_seq = estado_leer_u32("seq", 0);
    if (sd) {
        almacen_reparar_cola();
        uint32_t max = almacen_max_secuencia();
        if (max >= s_seq) {
            s_seq = max;
        }
    } else {
        audio_tono(TONO_ERROR);
    }

    xTaskCreate(tarea_sync, "sync", 12288, NULL, 4, &s_tarea_sync);
    actividad();

    switch (causa) {
    case DESPERTAR_ENCENDIDO:
        avisar_bateria();
        // Al encender: bajar/rotar imágenes (refresco ~20 s). Sin esto, con cola vacía
        // y sync_img reciente la pantalla no cambia sola.
        pedir_sync(SYNC_IMAGENES);
        break;
    case DESPERTAR_PWR:
        pedir_sync(SYNC_AVISAR);
        break;
    case DESPERTAR_TEMPORIZADOR:
        s_reposo_rapido = true;
        pedir_sync(SYNC_REVISAR);
        break;
    case DESPERTAR_BOOT:
        break;  // botones_iniciar ya ha registrado la pulsación
    }
    imagenes_rotar_si_toca();

    QueueHandle_t eventos = botones_cola();
    int64_t ultima_revision_us = ahora_us();
    for (;;) {
        evento_boton_t ev;
        if (xQueueReceive(eventos, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            actividad();
            manejar_boton(ev);
        }
        if (audio_grabando()) {
            actividad();
            if (audio_grabacion_al_maximo()) {
                terminar_grabacion(false);
            }
            continue;
        }
        int64_t t = ahora_us();
        if (t - ultima_revision_us > 30LL * 1000000) {
            ultima_revision_us = t;
            if (!s_sync_en_curso && t - s_ultimo_intento_us > (int64_t)g_ajustes.reintento_min * 60 * 1000000) {
                pedir_sync(0);
            }
            imagenes_rotar_si_toca();
        }
        int64_t reposo = (int64_t)(s_reposo_rapido ? REPOSO_RAPIDO_SEG : g_ajustes.reposo_seg) * 1000000;
        if (!s_sync_en_curso && !pantalla_ocupada() && t - s_actividad_us > reposo) {
            dormir();
        }
    }
}
