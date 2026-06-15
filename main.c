#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>

/* ─────────────────────────────────────────────
 * Pinos e constantes do sensor
 * ───────────────────────────────────────────── */
#define TRIG_PIN            GPIO_NUM_5
#define ECHO_PIN            GPIO_NUM_18

/*
 * Caixa d'água circular:
 *   Altura útil (sensor → fundo): 211 cm  (gap de instalação já descontado)
 *   Volume total:                 16.000 L
 *   Litros por centímetro:        16000 / 211 ≈ 75,83 L/cm
 */
#define TANK_HEIGHT_CM      211.0f   /* Altura útil em cm                  */
#define TANK_VOLUME_L       16000.0f /* Volume total em litros             */
#define TANK_L_PER_CM       (TANK_VOLUME_L / TANK_HEIGHT_CM) /* ~75,83 L/cm */

/* ─────────────────────────────────────────────
 * Configurações do banco de dados em memória
 * ───────────────────────────────────────────── */
#define DB_MAX_RECORDS  100         /* Capacidade máxima do histórico */
#define QUEUE_LENGTH    10          /* Fila entre as tasks            */

static const char *TAG_SENSOR = "SENSOR";
static const char *TAG_DB     = "DATABASE";

/* ─────────────────────────────────────────────
 * Estruturas de dados
 * ───────────────────────────────────────────── */

/** Registro enviado da task de leitura para a task de banco de dados */
typedef struct {
    int64_t  timestamp_us;    /* Tempo desde o boot em microssegundos  */
    float    distancia_cm;    /* Distância sensor → superfície da água */
    float    nivel_agua_cm;   /* Altura da coluna de água              */
    float    percentual;      /* 0–100 %                               */
    float    litros;          /* Volume atual em litros                */
} sensor_record_t;

/** Banco de dados circular em memória */
typedef struct {
    sensor_record_t registros[DB_MAX_RECORDS];
    uint32_t        total;    /* Total de registros inseridos (histórico) */
    uint16_t        head;     /* Próxima posição de escrita (circular)    */
    uint16_t        count;    /* Quantidade de registros válidos agora    */
} sensor_db_t;

/* ─────────────────────────────────────────────
 * Variáveis globais
 * ───────────────────────────────────────────── */
static QueueHandle_t  xSensorQueue;
static sensor_db_t    sensor_db;

/* ─────────────────────────────────────────────
 * Funções auxiliares do banco de dados
 * ───────────────────────────────────────────── */

/** Insere um novo registro no buffer circular */
static void db_insert(const sensor_record_t *rec)
{
    sensor_db.registros[sensor_db.head] = *rec;
    sensor_db.head = (sensor_db.head + 1) % DB_MAX_RECORDS;
    if (sensor_db.count < DB_MAX_RECORDS) {
        sensor_db.count++;
    }
    sensor_db.total++;
}

/** Imprime os últimos N registros (ou todos, se N > count) */
static void db_print_history(uint16_t n)
{
    if (sensor_db.count == 0) {
        ESP_LOGI(TAG_DB, "Histórico vazio.");
        return;
    }

    if (n > sensor_db.count) n = sensor_db.count;

    ESP_LOGI(TAG_DB, "──── Últimos %u registro(s) ────", n);

    /* Percorre do mais antigo ao mais recente dentro da janela pedida */
    for (uint16_t i = 0; i < n; i++) {
        /* Índice no buffer circular (do mais antigo ao mais recente) */
        uint16_t idx = (sensor_db.head - n + i + DB_MAX_RECORDS) % DB_MAX_RECORDS;
        const sensor_record_t *r = &sensor_db.registros[idx];

        ESP_LOGI(TAG_DB,
                 "[#%lu | t=%lld ms] Dist: %.1f cm | Nível: %.1f cm | %.1f%% | %.0f L",
                 (unsigned long)sensor_db.total - n + i + 1,
                 r->timestamp_us / 1000,
                 r->distancia_cm,
                 r->nivel_agua_cm,
                 r->percentual,
                 r->litros);
    }
    ESP_LOGI(TAG_DB, "────────────────────────────────");
}

/* ─────────────────────────────────────────────
 * Task 1 – Leitura do sensor ultrassônico
 * Período: 1 s
 * Prioridade: 5 (mais alta, pois exige timing preciso)
 * ───────────────────────────────────────────── */
static void vTaskSensor(void *pvParameters)
{
    /* Configura pinos */
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        /* 1. Dispara pulso de 10 µs no TRIG */
        gpio_set_level(TRIG_PIN, 0);
        ets_delay_us(2);
        gpio_set_level(TRIG_PIN, 1);
        ets_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        /* 2. Aguarda ECHO subir (com timeout de ~30 ms) */
        uint32_t timeout = 30000;
        while (gpio_get_level(ECHO_PIN) == 0 && timeout--) {
            ets_delay_us(1);
        }

        if (timeout == 0) {
            ESP_LOGW(TAG_SENSOR, "Timeout aguardando ECHO subir");
            vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
            continue;
        }

        /* 3. Conta o tempo que o ECHO fica em alto (µs) */
        long tempo = 0;
        while (gpio_get_level(ECHO_PIN) == 1) {
            ets_delay_us(1);
            tempo++;
            if (tempo > 38000) break; /* Distância máxima ~650 cm */
        }

        /* 4. Calcula distância, nível e volume */
        float distancia_cm  = tempo / 58.0f;
        float nivel_agua_cm = TANK_HEIGHT_CM - distancia_cm;
        if (nivel_agua_cm < 0.0f) nivel_agua_cm = 0.0f;
        if (nivel_agua_cm > TANK_HEIGHT_CM) nivel_agua_cm = TANK_HEIGHT_CM;
        float percentual = (nivel_agua_cm / TANK_HEIGHT_CM) * 100.0f;
        float litros     = nivel_agua_cm * TANK_L_PER_CM;

        ESP_LOGI(TAG_SENSOR,
                 "Distância: %.1f cm | Nível: %.1f cm | %.1f%% | %.0f L",
                 distancia_cm, nivel_agua_cm, percentual, litros);

        /* 5. Monta e envia o registro para a fila */
        sensor_record_t rec = {
            .timestamp_us  = esp_timer_get_time(),
            .distancia_cm  = distancia_cm,
            .nivel_agua_cm = nivel_agua_cm,
            .percentual    = percentual,
            .litros        = litros,
        };

        if (xQueueSend(xSensorQueue, &rec, 0) != pdTRUE) {
            ESP_LOGW(TAG_SENSOR, "Fila cheia – registro descartado");
        }

        /* 6. Aguarda até completar o período de 1 s */
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    }
}

/* ─────────────────────────────────────────────
 * Task 2 – Banco de dados / histórico
 * Aguarda itens na fila, armazena e exibe resumo
 * a cada 10 registros.
 * Prioridade: 3 (mais baixa – processa quando livre)
 * ───────────────────────────────────────────── */
static void vTaskDatabase(void *pvParameters)
{
    sensor_record_t rec;
    const uint16_t  PRINT_EVERY = 10; /* Exibe histórico a cada N leituras */

    memset(&sensor_db, 0, sizeof(sensor_db));

    while (1) {
        /* Bloqueia até receber um novo registro (sem timeout) */
        if (xQueueReceive(xSensorQueue, &rec, portMAX_DELAY) == pdTRUE) {
            db_insert(&rec);

            ESP_LOGI(TAG_DB,
                     "Registro #%lu salvo | %.1f%% | %.0f L | Total: %u/%u slots",
                     (unsigned long)sensor_db.total,
                     rec.percentual,
                     rec.litros,
                     sensor_db.count,
                     DB_MAX_RECORDS);

            /* Exibe histórico completo a cada PRINT_EVERY registros */
            if (sensor_db.total % PRINT_EVERY == 0) {
                db_print_history(PRINT_EVERY);
            }
        }
    }
}

/* ─────────────────────────────────────────────
 * Ponto de entrada
 * ───────────────────────────────────────────── */
void app_main(void)
{
    /* Cria a fila de comunicação entre as tasks */
    xSensorQueue = xQueueCreate(QUEUE_LENGTH, sizeof(sensor_record_t));
    if (xSensorQueue == NULL) {
        ESP_LOGE(TAG_SENSOR, "Falha ao criar a fila!");
        return;
    }

    /* Task de leitura do sensor – stack 2 KB, prioridade 5 */
    xTaskCreate(vTaskSensor,
                "sensor_task",
                2048,
                NULL,
                5,
                NULL);

    /* Task de banco de dados – stack 4 KB, prioridade 3 */
    xTaskCreate(vTaskDatabase,
                "db_task",
                4096,
                NULL,
                3,
                NULL);
}