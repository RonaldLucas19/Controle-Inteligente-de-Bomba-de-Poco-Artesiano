#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <rom/ets_sys.h>
 
#define TRIG_PIN    GPIO_NUM_5
#define ECHO_PIN    GPIO_NUM_18
 
#define TANK_HEIGHT_CM  211.0f  /* Altura total da caixa d'água em cm */
 
static const char *TAG = "ULTRASSONICO";
 
void app_main(void)
{
    /* Configura TRIG como saída */
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
 
    /* Configura ECHO como entrada */
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);
 
    while (1) {
        /* Dispara pulso de 10 µs no TRIG */
        gpio_set_level(TRIG_PIN, 0);
        ets_delay_us(2);
        gpio_set_level(TRIG_PIN, 1);
        ets_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);
 
        /* Aguarda ECHO subir */
        while (gpio_get_level(ECHO_PIN) == 0);
 
        /* Conta o tempo que o ECHO fica em alto */
        long tempo = 0;
        while (gpio_get_level(ECHO_PIN) == 1) {
            ets_delay_us(1);
            tempo++;
        }
 
        /* Calcula distância e nível */
        float distancia_cm  = tempo / 58.0f;
        float nivel_agua_cm = TANK_HEIGHT_CM - distancia_cm;
 
        if (nivel_agua_cm < 0) nivel_agua_cm = 0;
 
        float percentual = (nivel_agua_cm / TANK_HEIGHT_CM) * 100.0f;
 
        ESP_LOGI(TAG, "Distancia: %.1f cm | Nivel: %.1f cm | %.1f%%",
                 distancia_cm, nivel_agua_cm, percentual);
 
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}