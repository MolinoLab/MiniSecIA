#include "energia.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "placa.h"
#include "soc/soc_caps.h"

static const char *TAG = "energia";
static volatile modo_led_t s_modo_led = LED_APAGADO;

static const gpio_num_t PINES_RETENIDOS[] = {PIN_VBAT_PWR, PIN_AUDIO_PWR, PIN_EPD_PWR, PIN_LED};

static void configurar_salida(gpio_num_t pin, int nivel)
{
    gpio_set_level(pin, nivel);
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    gpio_set_level(pin, nivel);
}

static void tarea_led(void *arg)
{
    bool encendido = false;
    for (;;) {
        switch (s_modo_led) {
        case LED_FIJO:
            encendido = true;
            break;
        case LED_PARPADEO:
            encendido = !encendido;
            break;
        default:
            encendido = false;
        }
        gpio_set_level(PIN_LED, encendido ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(s_modo_led == LED_PARPADEO ? 250 : 100));
    }
}

despertar_t energia_iniciar(void)
{
    // Primero fijar los niveles y después soltar la retención, para que no haya glitch
    // en el enclavamiento de la batería.
    configurar_salida(PIN_VBAT_PWR, 1);
    configurar_salida(PIN_AUDIO_PWR, 1);
    configurar_salida(PIN_EPD_PWR, 1);
    configurar_salida(PIN_LED, 1);
    for (size_t i = 0; i < sizeof(PINES_RETENIDOS) / sizeof(PINES_RETENIDOS[0]); i++) {
        gpio_hold_dis(PINES_RETENIDOS[i]);
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif

    despertar_t causa = DESPERTAR_ENCENDIDO;
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    if (c == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t pines = esp_sleep_get_ext1_wakeup_status();
        causa = (pines & (1ULL << PIN_BOTON_BOOT)) ? DESPERTAR_BOOT : DESPERTAR_PWR;
    } else if (c == ESP_SLEEP_WAKEUP_TIMER) {
        causa = DESPERTAR_TEMPORIZADOR;
    }
    if (c != ESP_SLEEP_WAKEUP_UNDEFINED) {
        rtc_gpio_deinit(PIN_BOTON_BOOT);
        rtc_gpio_deinit(PIN_BOTON_PWR);
    }
    xTaskCreate(tarea_led, "led", 2048, NULL, 2, NULL);
    return causa;
}

void energia_audio(bool encendido)
{
    gpio_set_level(PIN_AUDIO_PWR, encendido ? 0 : 1);
}

void energia_pantalla(bool encendido)
{
    gpio_set_level(PIN_EPD_PWR, encendido ? 0 : 1);
}

void energia_led(modo_led_t modo)
{
    s_modo_led = modo;
}

int energia_bateria_mv(void)
{
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) {
        return 0;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &ccfg);

    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bool calibrado = adc_cali_create_scheme_curve_fitting(&cal_cfg, &cali) == ESP_OK;

    int suma = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int bruto, mv;
        if (adc_oneshot_read(adc, ADC_CHANNEL_3, &bruto) != ESP_OK) {
            continue;
        }
        if (calibrado && adc_cali_raw_to_voltage(cali, bruto, &mv) == ESP_OK) {
            suma += mv;
        } else {
            suma += bruto * 3100 / 4095;
        }
        n++;
    }
    if (calibrado) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    adc_oneshot_del_unit(adc);
    return n ? (suma / n) * 2 : 0;
}

// Con un botón pulsado, ext1 (nivel bajo) despertaría al instante.
static void esperar_botones_sueltos(void)
{
    for (int i = 0; i < 1000; i++) {
        if (gpio_get_level(PIN_BOTON_BOOT) && gpio_get_level(PIN_BOTON_PWR)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void dormir_con_botones(uint32_t segundos)
{
    esperar_botones_sueltos();
    const uint64_t botones = (1ULL << PIN_BOTON_BOOT) | (1ULL << PIN_BOTON_PWR);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_en(PIN_BOTON_BOOT);
    rtc_gpio_pulldown_dis(PIN_BOTON_BOOT);
    rtc_gpio_pullup_en(PIN_BOTON_PWR);
    rtc_gpio_pulldown_dis(PIN_BOTON_PWR);
    esp_sleep_enable_ext1_wakeup_io(botones, ESP_EXT1_WAKEUP_ANY_LOW);
    if (segundos > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)segundos * 1000000ULL);
    }
    esp_deep_sleep_start();
}

static void retener_pines(void)
{
    for (size_t i = 0; i < sizeof(PINES_RETENIDOS) / sizeof(PINES_RETENIDOS[0]); i++) {
        gpio_hold_en(PINES_RETENIDOS[i]);
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

void energia_dormir(uint32_t segundos)
{
    ESP_LOGI(TAG, "Sueño profundo (%lu s)", (unsigned long)segundos);
    s_modo_led = LED_APAGADO;
    gpio_set_level(PIN_LED, 1);
    energia_audio(false);
    energia_pantalla(false);
    retener_pines();
    dormir_con_botones(segundos);
}

void energia_apagar(void)
{
    ESP_LOGI(TAG, "Apagando");
    energia_audio(false);
    energia_pantalla(false);
    gpio_set_level(PIN_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_VBAT_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Seguimos vivos: alimentados por USB. Dormir con la batería desenclavada.
    ESP_LOGW(TAG, "Alimentación USB: se duerme en lugar de apagar");
    s_modo_led = LED_APAGADO;
    retener_pines();
    dormir_con_botones(0);
}
