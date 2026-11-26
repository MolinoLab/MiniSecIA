// MiniSecIA: grabadora de bolsillo que envía notas de voz a ConvocAUTOrias (POST /voz).
//
// BOOT corto: grabar / terminar. Durante una escucha, la anterior.
// PWR corto:  cancelar la grabación, o sincronizar. Durante una escucha, la siguiente
//             (si ya es la última, para).
// BOOT largo: la escucha más reciente, incluidas las ya oídas (sin Wi-Fi)
// PWR largo:  apagar
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
#include "escucha.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imagenes.h"
#include "meditacion.h"
#include "nvs_flash.h"
#include "realidad.h"
#include "pantalla.h"
#include "placa.h"
#include "red.h"
#include "reloj.h"

static const char *TAG = "main";

#define SYNC_AVISAR   (1 << 0)  // sonido con el resultado
#define SYNC_IMAGENES (1 << 1)  // forzar descarga de imágenes y avanzar a la siguiente (manual)
#define SYNC_REVISAR  (1 << 2)  // despertar la tarea de sync; no usar 0 con eSetBits
#define SYNC_SOLO_SUBIR (1 << 3)  // un intento de subir la cola, sin bajar audios ni imágenes
#define LOTE_COLA 16
#define BYTES_MIN_GRABACION (AUDIO_HZ * 2 / 2)  // 0,5 s
#define BATERIA_BAJA_MV 3450
#define REPOSO_RAPIDO_SEG 5
#define CLAVE_SUBIDA_OK "subida_ok"

static i2c_master_bus_handle_t s_i2c = NULL;
static TaskHandle_t s_tarea_sync = NULL;
static volatile bool s_sync_en_curso = false;
static volatile bool s_no_autorizado = false;
static volatile int64_t s_actividad_us = 0;
static bool s_reposo_rapido = false;
static bool s_audio_ok = false;
static bool s_hablo_respuesta = false;  // clima u otra respuesta Opus; no hace falta el tono OK
static uint32_t s_seq = 0;
static uint32_t s_instancia = 0;
static char s_grabacion[RUTA_MAX];

// true = la última subida llegó al servidor; tras grabar se puede intentar otra vez.
// false = falló (sin red, etc.): solo se reintenta con PWR corto.
static bool ultima_subida_ok(void)
{
    return estado_leer_u32(CLAVE_SUBIDA_OK, 1) != 0;
}

static void marcar_subida_ok(bool ok)
{
    estado_guardar_u32(CLAVE_SUBIDA_OK, ok ? 1 : 0);
}

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
    s_hablo_respuesta = false;
    for (;;) {
        int total = 0;
        int n = almacen_listar_cola(rutas, LOTE_COLA, &total);
        if (n == 0) {
            return ultimo;
        }
        for (int i = 0; i < n; i++) {
            char rid[80], iso[24];
            char audio_path[96] = {0};
            int med_min = 0;
            request_id(rutas[i], rid, sizeof(rid), iso, sizeof(iso));
            resultado_subida_t r = cliente_subir(rutas[i], rid, iso, audio_path, sizeof(audio_path), &med_min);
            ultimo = r;
            actividad();
            if (r == SUBIDA_OK && audio_path[0]) {
                // Clima u otra respuesta hablada: reproducir y borrar (no va a /escucha).
                if (cliente_reproducir_respuesta(audio_path)) {
                    s_hablo_respuesta = true;
                }
                actividad();
            } else if (r == SUBIDA_MEDITACION) {
                meditacion_lanzar_ahora(med_min);
                s_hablo_respuesta = true;  // no pitido OK encima del tono de meditación
                s_reposo_rapido = true;    // dormir pronto para despertar al fin
                actividad();
            } else if (r == SUBIDA_REALIDAD) {
                realidad_lanzar_ahora();
                s_hablo_respuesta = true;  // no pitido OK encima del chirp
                actividad();
            }
            switch (r) {
            case SUBIDA_OK:
            case SUBIDA_PENDIENTE:
            case SUBIDA_MEDITACION:
            case SUBIDA_REALIDAD:
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
        bool solo_subir = bits & SYNC_SOLO_SUBIR;
        int en_cola = almacen_montada() ? almacen_contar_cola() : 0;
        bool img = !solo_subir && almacen_montada() && (forzar_img || imagenes_toca_sincronizar());
        // PWR corto baja audios. Tras grabar (si subida_ok) solo se intenta la cola.
        bool bajar = avisar && !solo_subir && almacen_montada();
        ESP_LOGI(TAG, "Sync bits=0x%lx cola=%d img=%d bajar=%d", (unsigned long)bits, en_cola, img ? 1 : 0,
                 bajar ? 1 : 0);
        if (en_cola == 0 && !img && !bajar) {
            continue;
        }
        s_sync_en_curso = true;
        actividad();

        if (!ajustes_servidor_configurado() || s_no_autorizado) {
            ESP_LOGW(TAG, "Servidor o Wi-Fi sin configurar (o token rechazado)");
            marcar_subida_ok(false);
            if (avisar) {
                audio_tono(TONO_ERROR);
            }
        } else {
            if (!audio_grabando()) {
                energia_led(LED_PARPADEO);
            }
            if (!red_conectar()) {
                marcar_subida_ok(false);
                if (avisar) {
                    audio_tono(TONO_EN_COLA);
                }
            } else {
                int r = en_cola ? subir_cola() : -1;
                if (r == SUBIDA_REINTENTAR || r == SUBIDA_ERROR_CONFIG) {
                    marcar_subida_ok(false);
                } else if (en_cola > 0) {
                    // OK / pendiente / vacía / rechazada: el servidor respondió.
                    marcar_subida_ok(true);
                }
                if (bajar && !s_no_autorizado) {
                    escucha_sincronizar();
                    meditacion_sincronizar();
                    realidad_sincronizar();
                }
                if (img && !s_no_autorizado) {
                    imagenes_sincronizar(forzar_img);
                }
                red_desconectar();
                if (avisar && !audio_grabando() && !audio_reproduciendo()) {
                    if (s_hablo_respuesta &&
                        (r == SUBIDA_OK || r == SUBIDA_MEDITACION || r == SUBIDA_REALIDAD ||
                         r < 0)) {
                        // Ya sonó clima Opus, meditación o chirp de realidad.
                    } else if (r < 0) {
                        audio_tono(TONO_OK);
                    } else {
                        tono_resultado(r);
                    }
                }
            }
            if (!audio_grabando()) {
                energia_led(LED_APAGADO);
            }
        }
        if (forzar_img && !solo_subir) {
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

static void terminar_grabacion(bool cancelar, bool intentar_subir)
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
    // Solo reabre Wi-Fi si la última subida fue bien. Si falló, espera a PWR corto.
    if (intentar_subir && ultima_subida_ok()) {
        pedir_sync(SYNC_AVISAR | SYNC_SOLO_SUBIR);
    }
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
        terminar_grabacion(false, false);
    }
    if (escucha_activa()) {
        escucha_parar();
        for (int i = 0; i < 50 && escucha_activa(); i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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
    // El Wi-Fi no se busca solo: la cola espera a un PWR corto. El temporizador
    // despierta para rotar la imagen, meditación o checkeo de realidad.
    uint32_t rot = imagenes_segundos_hasta_rotacion();
    uint32_t seg = meditacion_preparar_sueno(rot);
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
        if (escucha_activa()) {
            escucha_anterior();
            break;
        }
        if (audio_grabando()) {
            terminar_grabacion(false, true);
        } else {
            empezar_grabacion();
        }
        break;
    case EV_PWR_CORTO:
        if (audio_grabando()) {
            terminar_grabacion(true, false);
        } else if (escucha_activa()) {
            escucha_siguiente();
        } else {
            // Sync de cola/audios/manifiesto de imágenes; la foto de pantalla
            // solo rota por el periodo imagen_horas, no aquí.
            pedir_sync(SYNC_AVISAR);
            avisar_bateria();
        }
        break;
    case EV_BOOT_LARGO:
        if (!audio_grabando()) {
            escucha_empezar();
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
    meditacion_iniciar();
    realidad_iniciar();

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
        break;
    case DESPERTAR_PWR:
        break;  // la sync es un PWR corto ya despierto
    case DESPERTAR_TEMPORIZADOR:
        s_reposo_rapido = true;
        meditacion_al_despertar();
        realidad_al_despertar();
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
        if (audio_grabando() || escucha_activa()) {
            actividad();
        }
        if (audio_grabando()) {
            if (audio_grabacion_al_maximo()) {
                terminar_grabacion(false, true);
            }
            continue;
        }
        // Durante una escucha no rotar e-Paper ni dormir: el refresco (~20 s)
        // lee la SD a la vez que el Opus y corta la reproducción.
        if (escucha_activa()) {
            continue;
        }
        int64_t t = ahora_us();
        if (t - ultima_revision_us > 30LL * 1000000) {
            ultima_revision_us = t;
            imagenes_rotar_si_toca();
        }
        int64_t reposo = (int64_t)(s_reposo_rapido ? REPOSO_RAPIDO_SEG : g_ajustes.reposo_seg) * 1000000;
        if (!s_sync_en_curso && !pantalla_ocupada() && t - s_actividad_us > reposo) {
            dormir();
        }
    }
}
