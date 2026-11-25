#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "almacen.h"
#include "driver/i2s_std.h"
#include "energia.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
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
static volatile bool s_reproduciendo = false;
static volatile bool s_parar_repro = false;
static volatile bool s_al_maximo = false;
static volatile bool s_error_escritura = false;
static uint32_t s_max_bytes = 0;
static uint32_t s_bytes_escritos = 0;
static uint32_t s_perdidos = 0;
static FILE *s_fichero = NULL;
static char s_ruta[RUTA_MAX];
static int s_volumen = 70;

#define GANANCIA_REPRO 2
#define MAX_OPUS_RAM (3 * 1024 * 1024)

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
    s_volumen = volumen;
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
// Cuenco suave: más grave y espaciado que OK/sync, inconfundible.
static const nota_t T_MEDITACION[] = {
    {392, 280}, {0, 120}, {523, 320}, {0, 160}, {392, 400}, FIN_NOTAS};
static const nota_t T_MEDITACION_FIN[] = {
    {523, 200}, {0, 80}, {392, 260}, {0, 100}, {330, 360}, FIN_NOTAS};

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
        [TONO_MEDITACION] = T_MEDITACION,
        [TONO_MEDITACION_FIN] = T_MEDITACION_FIN,
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

// --- Reproducción Ogg Opus ---

static int s_out_n = 0;
static int s_rs_acc = 0;
static int16_t s_out_estereo[FRAMES_BLOQUE * 2];

static void vaciar_salida(void)
{
    if (s_out_n > 0 && s_salida) {
        esp_codec_dev_write(s_salida, s_out_estereo, s_out_n * 4);
        s_out_n = 0;
    }
}

static void empujar_muestra(int16_t mono)
{
    s_out_estereo[s_out_n * 2] = mono;
    s_out_estereo[s_out_n * 2 + 1] = mono;
    s_out_n++;
    if (s_out_n >= FRAMES_BLOQUE) {
        vaciar_salida();
    }
}

// Lleva PCM del decoder (cualquier tasa habitual de Opus) a 16 kHz estéreo del ES8311.
static void alimentar_pcm(const int16_t *pcm, int bytes, int canales, int hz)
{
    if (hz <= 0 || canales <= 0 || bytes < canales * 2) {
        return;
    }
    int frames = bytes / (canales * 2);
    for (int i = 0; i < frames && !s_parar_repro; i++) {
        int32_t suma = 0;
        for (int c = 0; c < canales; c++) {
            suma += pcm[i * canales + c];
        }
        int32_t mono32 = (suma / canales) * GANANCIA_REPRO;
        if (mono32 > 32767) {
            mono32 = 32767;
        } else if (mono32 < -32768) {
            mono32 = -32768;
        }
        int16_t mono = (int16_t)mono32;
        s_rs_acc += AUDIO_HZ;
        while (s_rs_acc >= hz && !s_parar_repro) {
            empujar_muestra(mono);
            s_rs_acc -= hz;
        }
    }
}

void audio_reproducir_parar(void)
{
    s_parar_repro = true;
}

void audio_reproduccion_ocupar(bool ocupada)
{
    s_reproduciendo = ocupada;
}

bool audio_reproduciendo(void)
{
    return s_reproduciendo;
}

bool audio_reproducir_opus(const char *ruta)
{
    if (!s_salida || !ruta || s_grabando) {
        s_reproduciendo = false;
        return false;
    }
    FILE *f = fopen(ruta, "rb");
    if (!f) {
        ESP_LOGE(TAG, "No se puede abrir %s", ruta);
        s_reproduciendo = false;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        s_reproduciendo = false;
        return false;
    }
    long tam_l = ftell(f);
    if (tam_l <= 0) {
        ESP_LOGE(TAG, "Opus vacio");
        fclose(f);
        s_reproduciendo = false;
        return false;
    }
    size_t tam = (size_t)tam_l;
    rewind(f);

    // Preferimos PSRAM (sin pelear con e-Paper/SD). Si no cabe o no hay memoria,
    // se decodifica leyendo de la SD a trozos.
    uint8_t *archivo = NULL;
    bool en_ram = false;
    if (tam <= MAX_OPUS_RAM) {
        archivo = heap_caps_malloc(tam, MALLOC_CAP_SPIRAM);
        if (archivo && fread(archivo, 1, tam, f) == tam) {
            en_ram = true;
            fclose(f);
            f = NULL;
            ESP_LOGI(TAG, "Opus en PSRAM (%u bytes)", (unsigned)tam);
        } else {
            free(archivo);
            archivo = NULL;
            rewind(f);
            ESP_LOGW(TAG, "Sin PSRAM para %u bytes; se lee desde SD", (unsigned)tam);
        }
    } else {
        ESP_LOGW(TAG, "Opus %u bytes > %d; se lee desde SD", (unsigned)tam, MAX_OPUS_RAM);
    }

    static bool registrados = false;
    if (!registrados) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registrados = true;
    }
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "No se pudo abrir el decoder Opus");
        free(archivo);
        if (f) {
            fclose(f);
        }
        s_reproduciendo = false;
        return false;
    }

    bool saltar = s_parar_repro;
    s_parar_repro = false;
    s_reproduciendo = true;
    if (saltar) {
        esp_audio_simple_dec_close(dec);
        free(archivo);
        if (f) {
            fclose(f);
        }
        s_reproduciendo = false;
        return true;
    }
    s_out_n = 0;
    s_rs_acc = 0;
    bool ok = true;
    int info_hz = AUDIO_HZ;
    int info_ch = 1;
    bool info_lista = false;
    int max_out = 16 * 1024;
    uint8_t *salida = heap_caps_malloc(max_out, MALLOC_CAP_SPIRAM);
    int cap_in = 16 * 1024;
    uint8_t *entrada = en_ram ? NULL : heap_caps_malloc(cap_in, MALLOC_CAP_SPIRAM);
    if (!salida || (!en_ram && !entrada)) {
        esp_audio_simple_dec_close(dec);
        free(archivo);
        free(salida);
        free(entrada);
        if (f) {
            fclose(f);
        }
        s_reproduciendo = false;
        return false;
    }

    xSemaphoreTake(s_mutex_salida, portMAX_DELAY);
    esp_codec_dev_set_out_vol(s_salida, 90);
    esp_codec_dev_set_out_mute(s_salida, false);

    if (en_ram) {
        size_t pos = 0;
        while (pos < tam && !s_parar_repro) {
            size_t pos_antes = pos;
            esp_audio_simple_dec_raw_t raw = {
                .buffer = archivo + pos,
                .len = (uint32_t)(tam - pos),
                .eos = true,
            };
            while (raw.len && !s_parar_repro) {
                esp_audio_simple_dec_out_t out = {.buffer = salida, .len = (uint32_t)max_out};
                int ret = esp_audio_simple_dec_process(dec, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > (uint32_t)max_out) {
                    uint8_t *nuevo = heap_caps_realloc(salida, out.needed_size, MALLOC_CAP_SPIRAM);
                    if (!nuevo) {
                        ok = false;
                        raw.len = 0;
                        break;
                    }
                    salida = nuevo;
                    max_out = (int)out.needed_size;
                    continue;
                }
                if (ret == ESP_AUDIO_ERR_DATA_LACK || ret == ESP_AUDIO_ERR_CONTINUE) {
                    if (raw.consumed > 0 && raw.consumed <= raw.len) {
                        pos += raw.consumed;
                        raw.len -= raw.consumed;
                        raw.buffer += raw.consumed;
                        continue;
                    }
                    break;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Decoder Opus: %d (se sigue)", ret);
                    if (raw.consumed > 0 && raw.consumed <= raw.len) {
                        pos += raw.consumed;
                        raw.len -= raw.consumed;
                        raw.buffer += raw.consumed;
                    } else {
                        pos++;
                        if (raw.len) {
                            raw.buffer++;
                            raw.len--;
                        }
                    }
                    continue;
                }
                if (out.decoded_size && !info_lista) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate > 0) {
                        info_hz = info.sample_rate;
                        info_ch = info.channel > 0 ? info.channel : 1;
                        info_lista = true;
                        ESP_LOGI(TAG, "Opus %d Hz, %d canales", info_hz, info_ch);
                    }
                }
                if (out.decoded_size) {
                    alimentar_pcm((const int16_t *)out.buffer, (int)out.decoded_size, info_ch, info_hz);
                }
                if (raw.consumed == 0) {
                    break;
                }
                pos += raw.consumed;
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }
            if (pos == pos_antes) {
                break;
            }
        }
    } else {
        int pendiente = 0;
        for (;;) {
            if (s_parar_repro) {
                break;
            }
            if (cap_in - pendiente < 2048 && cap_in < 64 * 1024) {
                uint8_t *mas = heap_caps_realloc(entrada, cap_in * 2, MALLOC_CAP_SPIRAM);
                if (mas) {
                    entrada = mas;
                    cap_in *= 2;
                }
            }
            int leidos = (int)fread(entrada + pendiente, 1, (size_t)(cap_in - pendiente), f);
            if (leidos < 0) {
                leidos = 0;
            }
            esp_audio_simple_dec_raw_t raw = {
                .buffer = entrada,
                .len = (uint32_t)(pendiente + leidos),
                .eos = leidos == 0,
            };
            if (raw.len == 0) {
                break;
            }
            bool falta = false;
            while (raw.len && !s_parar_repro) {
                esp_audio_simple_dec_out_t out = {.buffer = salida, .len = (uint32_t)max_out};
                int ret = esp_audio_simple_dec_process(dec, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > (uint32_t)max_out) {
                    uint8_t *nuevo = heap_caps_realloc(salida, out.needed_size, MALLOC_CAP_SPIRAM);
                    if (!nuevo) {
                        ok = false;
                        raw.len = 0;
                        break;
                    }
                    salida = nuevo;
                    max_out = (int)out.needed_size;
                    continue;
                }
                if (ret == ESP_AUDIO_ERR_DATA_LACK || ret == ESP_AUDIO_ERR_CONTINUE) {
                    if (raw.consumed > 0 && raw.consumed <= raw.len) {
                        raw.len -= raw.consumed;
                        raw.buffer += raw.consumed;
                    }
                    falta = true;
                    break;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Decoder Opus: %d (se sigue)", ret);
                    if (raw.consumed > 0 && raw.consumed <= raw.len) {
                        raw.len -= raw.consumed;
                        raw.buffer += raw.consumed;
                    } else if (raw.len) {
                        raw.buffer++;
                        raw.len--;
                    } else {
                        break;
                    }
                    continue;
                }
                if (out.decoded_size && !info_lista) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate > 0) {
                        info_hz = info.sample_rate;
                        info_ch = info.channel > 0 ? info.channel : 1;
                        info_lista = true;
                        ESP_LOGI(TAG, "Opus %d Hz, %d canales (SD)", info_hz, info_ch);
                    }
                }
                if (out.decoded_size) {
                    alimentar_pcm((const int16_t *)out.buffer, (int)out.decoded_size, info_ch, info_hz);
                }
                if (raw.consumed == 0) {
                    falta = true;
                    break;
                }
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }
            pendiente = (int)raw.len;
            if (pendiente > 0 && raw.buffer != entrada) {
                memmove(entrada, raw.buffer, (size_t)pendiente);
            }
            if (s_parar_repro || !ok) {
                break;
            }
            if (leidos == 0) {
                break;
            }
            if (falta && pendiente >= cap_in) {
                ESP_LOGE(TAG, "El decoder no consume datos");
                ok = false;
                break;
            }
        }
    }

    vaciar_salida();
    nota_t silencio = {0, 80};
    escribir_nota(&silencio);
    esp_codec_dev_set_out_mute(s_salida, true);
    esp_codec_dev_set_out_vol(s_salida, s_volumen);
    xSemaphoreGive(s_mutex_salida);

    free(salida);
    free(entrada);
    free(archivo);
    if (f) {
        fclose(f);
    }
    esp_audio_simple_dec_close(dec);
    s_reproduciendo = false;
    s_parar_repro = false;
    ESP_LOGI(TAG, "Reproducción %s (%s, %s)", ruta, ok ? "completa" : "interrumpida",
             en_ram ? "PSRAM" : "SD");
    return ok;
}
