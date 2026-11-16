#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "almacen.h"
#include "driver/i2s_std.h"
#include "energia.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "placa.h"

static const char *TAG = "audio";

#define FRAMES_BLOQUE 512                       // 32 ms a 16 kHz
#define BYTES_ESTEREO (FRAMES_BLOQUE * 2 * 2)
#define BYTES_MONO (FRAMES_BLOQUE * 2)
#define TAM_FLUJO (256 * 1024)                  // ~8 s de margen si la SD se atasca
#define TAM_ESCRITURA (16 * 1024)
#define DESCARTE_INICIAL_MS 120                 // cola del pitido y DMA antiguo

#define BIT_LECTOR_FIN BIT0
#define BIT_ESCRITOR_FIN BIT1

static esp_codec_dev_handle_t s_salida = NULL;
static esp_codec_dev_handle_t s_entrada = NULL;
static SemaphoreHandle_t s_mutex_salida = NULL;

static StreamBufferHandle_t s_flujo = NULL;
static EventGroupHandle_t s_eventos = NULL;
static uint8_t *s_buf_estereo = NULL;
static uint8_t *s_buf_tono = NULL;
static uint8_t *s_buf_mono = NULL;
static uint8_t *s_buf_escritura = NULL;

static volatile bool s_grabando = false;
static volatile bool s_al_maximo = false;
static volatile bool s_error_escritura = false;
static uint32_t s_max_bytes = 0;
static uint32_t s_bytes_escritos = 0;
static uint32_t s_perdidos = 0;
static FILE *s_fichero = NULL;
static char s_ruta[RUTA_MAX];

bool audio_iniciar(i2c_master_bus_handle_t bus, int volumen, int ganancia_db)
{
    energia_audio(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;  // silencio cuando no hay nada que reproducir
    if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel");
        return false;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    i2s_channel_init_std_mode(tx, &std);
    i2s_channel_init_std_mode(rx, &std);

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        ESP_LOGE(TAG, "No se pudieron crear las interfaces del códec");
        return false;
    }
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PIN_PA,
        .use_mclk = true,
        .hw_gain = {.pa_gain = 6},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 no responde");
        return false;
    }
    esp_codec_dev_cfg_t dev = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    s_salida = esp_codec_dev_new(&dev);
    dev.dev_type = ESP_CODEC_DEV_TYPE_IN;
    s_entrada = esp_codec_dev_new(&dev);

    // Mismo formato en los dos sentidos (comparten el I2S). Estéreo porque el ES8311 entrega
    // el micrófono en el canal izquierdo y la referencia del DAC en el derecho.
    esp_codec_dev_sample_info_t fs = {.sample_rate = AUDIO_HZ, .channel = 2, .bits_per_sample = 16};
    if (esp_codec_dev_open(s_salida, &fs) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(s_entrada, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "No se pudo abrir el códec");
        return false;
    }
    esp_codec_dev_set_out_vol(s_salida, volumen);
    esp_codec_dev_set_in_gain(s_entrada, (float)ganancia_db);
    esp_codec_dev_set_out_mute(s_salida, true);

    s_mutex_salida = xSemaphoreCreateMutex();
    s_eventos = xEventGroupCreate();
    s_flujo = xStreamBufferCreateWithCaps(TAM_FLUJO, 1, MALLOC_CAP_SPIRAM);
    s_buf_estereo = heap_caps_malloc(BYTES_ESTEREO, MALLOC_CAP_SPIRAM);
    s_buf_tono = heap_caps_malloc(BYTES_ESTEREO, MALLOC_CAP_SPIRAM);
    s_buf_mono = heap_caps_malloc(BYTES_MONO, MALLOC_CAP_SPIRAM);
    s_buf_escritura = heap_caps_malloc(TAM_ESCRITURA, MALLOC_CAP_SPIRAM);
    if (!s_flujo || !s_buf_estereo || !s_buf_tono || !s_buf_mono || !s_buf_escritura) {
        ESP_LOGE(TAG, "Sin memoria para los buffers de audio");
        return false;
    }
    return true;
}

void audio_apagar(void)
{
    if (s_grabando) {
        audio_grabar_parar(false);
    }
    if (s_salida) {
        esp_codec_dev_close(s_salida);
    }
    if (s_entrada) {
        esp_codec_dev_close(s_entrada);
    }
    energia_audio(false);
}

// --- Tonos ---

typedef struct {
    uint16_t hz;  // 0 = silencio
    uint16_t ms;
} nota_t;

#define FIN_NOTAS {0, 0}

static const nota_t T_INICIO[] = {{880, 120}, FIN_NOTAS};
static const nota_t T_FIN[] = {{660, 80}, {440, 140}, FIN_NOTAS};
static const nota_t T_OK[] = {{660, 90}, {0, 30}, {880, 90}, {0, 30}, {1320, 160}, FIN_NOTAS};
static const nota_t T_PENDIENTE[] = {{880, 120}, {0, 80}, {880, 120}, FIN_NOTAS};
static const nota_t T_ERROR[] = {{220, 450}, FIN_NOTAS};
static const nota_t T_EN_COLA[] = {{523, 150}, {0, 50}, {392, 220}, FIN_NOTAS};
static const nota_t T_CANCELADO[] = {{440, 80}, {0, 40}, {330, 80}, {0, 40}, {220, 200}, FIN_NOTAS};
static const nota_t T_APAGADO[] = {{880, 110}, {660, 110}, {440, 110}, {220, 300}, FIN_NOTAS};
static const nota_t T_BATERIA[] = {{1760, 60}, {0, 60}, {1760, 60}, {0, 60}, {1760, 60}, FIN_NOTAS};

static void escribir_nota(const nota_t *n)
{
    int16_t *m = (int16_t *)s_buf_tono;
    uint32_t total = (uint32_t)n->ms * AUDIO_HZ / 1000;
    uint32_t rampa = AUDIO_HZ / 200;  // 5 ms de entrada y salida, sin chasquidos
    float fase = 0.0f, paso = 2.0f * (float)M_PI * n->hz / AUDIO_HZ;
    for (uint32_t hecho = 0; hecho < total;) {
        uint32_t k = total - hecho < FRAMES_BLOQUE ? total - hecho : FRAMES_BLOQUE;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t pos = hecho + i;
            float env = 1.0f;
            if (pos < rampa) {
                env = (float)pos / rampa;
            } else if (total - pos < rampa) {
                env = (float)(total - pos) / rampa;
            }
            int16_t v = n->hz ? (int16_t)(sinf(fase) * 12000.0f * env) : 0;
            fase += paso;
            if (fase > 2.0f * (float)M_PI) {
                fase -= 2.0f * (float)M_PI;
            }
            m[2 * i] = v;
            m[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_salida, s_buf_tono, (int)(k * 4));
        hecho += k;
    }
}

static void reproducir(const nota_t *notas)
{
    if (!s_salida || !s_buf_tono || audio_grabando()) {
        return;
    }
    xSemaphoreTake(s_mutex_salida, portMAX_DELAY);
    esp_codec_dev_set_out_mute(s_salida, false);
    for (const nota_t *n = notas; n->ms; n++) {
        escribir_nota(n);
    }
    nota_t silencio = {0, 120};  // vacía la DMA antes de silenciar
    escribir_nota(&silencio);
    esp_codec_dev_set_out_mute(s_salida, true);
    xSemaphoreGive(s_mutex_salida);
}

void audio_tono(tono_t tono)
{
    static const nota_t *const TABLA[] = {
        [TONO_INICIO] = T_INICIO,       [TONO_FIN] = T_FIN,
        [TONO_OK] = T_OK,               [TONO_PENDIENTE] = T_PENDIENTE,
        [TONO_ERROR] = T_ERROR,         [TONO_EN_COLA] = T_EN_COLA,
        [TONO_CANCELADO] = T_CANCELADO, [TONO_APAGADO] = T_APAGADO,
        [TONO_BATERIA_BAJA] = T_BATERIA,
    };
    reproducir(TABLA[tono]);
}

void audio_pitidos(int n)
{
    nota_t notas[2 * 10 + 1];
    int k = 0;
    if (n > 10) {
        n = 10;
    }
    for (int i = 0; i < n; i++) {
        notas[k++] = (nota_t){1000, 70};
        notas[k++] = (nota_t){0, 130};
    }
    notas[k] = (nota_t)FIN_NOTAS;
    reproducir(notas);
}

// --- Grabación ---

static void tarea_lector(void *arg)
{
    const int16_t *e = (const int16_t *)s_buf_estereo;
    int16_t *m = (int16_t *)s_buf_mono;
    uint32_t descartar = DESCARTE_INICIAL_MS * AUDIO_HZ / 1000 * 4;
    uint32_t enviados = 0;
    while (s_grabando) {
        if (esp_codec_dev_read(s_entrada, s_buf_estereo, BYTES_ESTEREO) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (descartar) {
            descartar = descartar > BYTES_ESTEREO ? descartar - BYTES_ESTEREO : 0;
            continue;
        }
        for (int i = 0; i < FRAMES_BLOQUE; i++) {
            m[i] = e[2 * i];  // canal izquierdo: micrófono
        }
        size_t n = xStreamBufferSend(s_flujo, m, BYTES_MONO, 0);
        if (n < BYTES_MONO) {
            s_perdidos += BYTES_MONO - n;
        }
        enviados += n;
        if (enviados >= s_max_bytes) {
            s_al_maximo = true;
            break;
        }
    }
    xEventGroupSetBits(s_eventos, BIT_LECTOR_FIN);
    vTaskDelete(NULL);
}

static void tarea_escritor(void *arg)
{
    for (;;) {
        size_t n = xStreamBufferReceive(s_flujo, s_buf_escritura, TAM_ESCRITURA, pdMS_TO_TICKS(50));
        if (n > 0) {
            if (fwrite(s_buf_escritura, 1, n, s_fichero) != n) {
                s_error_escritura = true;
                s_grabando = false;
            } else {
                s_bytes_escritos += n;
            }
            continue;
        }
        if ((xEventGroupGetBits(s_eventos) & BIT_LECTOR_FIN) && xStreamBufferIsEmpty(s_flujo)) {
            break;
        }
    }
    xEventGroupSetBits(s_eventos, BIT_ESCRITOR_FIN);
    vTaskDelete(NULL);
}

bool audio_grabar_iniciar(const char *ruta, int max_seg)
{
    if (s_grabando || !s_entrada || !almacen_montada()) {
        return false;
    }
    s_fichero = fopen(ruta, "wb");
    if (!s_fichero) {
        ESP_LOGE(TAG, "No se puede crear %s", ruta);
        return false;
    }
    setvbuf(s_fichero, NULL, _IOFBF, 8192);
    uint8_t cab[WAV_CABECERA];
    wav_cabecera(cab, 0);
    fwrite(cab, 1, sizeof(cab), s_fichero);

    strlcpy(s_ruta, ruta, sizeof(s_ruta));
    s_max_bytes = (uint32_t)max_seg * AUDIO_HZ * 2;
    s_bytes_escritos = 0;
    s_perdidos = 0;
    s_al_maximo = false;
    s_error_escritura = false;
    xStreamBufferReset(s_flujo);
    xEventGroupClearBits(s_eventos, BIT_LECTOR_FIN | BIT_ESCRITOR_FIN);
    s_grabando = true;
    xTaskCreatePinnedToCore(tarea_lector, "lector", 4096, NULL, 8, NULL, 1);
    xTaskCreatePinnedToCore(tarea_escritor, "escritor", 6144, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Grabando en %s", ruta);
    return true;
}

uint32_t audio_grabar_parar(bool cancelar)
{
    if (!s_fichero) {
        return 0;
    }
    s_grabando = false;
    EventBits_t b = xEventGroupWaitBits(s_eventos, BIT_LECTOR_FIN | BIT_ESCRITOR_FIN, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(15000));
    if ((b & (BIT_LECTOR_FIN | BIT_ESCRITOR_FIN)) != (BIT_LECTOR_FIN | BIT_ESCRITOR_FIN)) {
        ESP_LOGE(TAG, "Las tareas de grabación no terminaron");
    }
    uint32_t datos = s_bytes_escritos & ~1u;
    uint8_t cab[WAV_CABECERA];
    wav_cabecera(cab, datos);
    fseek(s_fichero, 0, SEEK_SET);
    fwrite(cab, 1, sizeof(cab), s_fichero);
    bool cerrado = fclose(s_fichero) == 0;
    s_fichero = NULL;

    if (s_perdidos) {
        ESP_LOGW(TAG, "Se perdieron %lu bytes (SD lenta)", (unsigned long)s_perdidos);
    }
    if (cancelar || s_error_escritura || !cerrado) {
        if (s_error_escritura) {
            ESP_LOGE(TAG, "Error escribiendo en la SD");
        }
        remove(s_ruta);
        return 0;
    }
    ESP_LOGI(TAG, "Grabación terminada: %.1f s", datos / (2.0 * AUDIO_HZ));
    return datos;
}

bool audio_grabando(void)
{
    return s_grabando || s_fichero != NULL;
}

bool audio_grabacion_al_maximo(void)
{
    return s_al_maximo;
}
