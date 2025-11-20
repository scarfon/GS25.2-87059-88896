#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"


#define LOG_PREFIX "{Andres Marcos | 87059 88896}"
#define WDT_TIMEOUT_MS            6000
#define SSID_MAX_LENGTH           32
#define SECURE_LIST_SIZE          5
#define QUEUE_LENGTH              6
#define WIFI_MONITOR_DELAY_MS     3000
#define QUEUE_SEND_TIMEOUT_MS     1000
#define CHECKER_QUEUE_TIMEOUT_MS  4000
#define SEMAPHORE_TIMEOUT_MS      1000
#define SUPERVISOR_DELAY_MS       2000
#define MAX_QUEUE_SEND_RETRIES    3
#define CHECKER_TIMEOUT_WARN_LIMIT   3
#define CHECKER_TIMEOUT_RESET_LIMIT  6

#define WIFI_STA_SSID      "saf"
#define WIFI_STA_PASSWORD  "1234567890"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/*
 * ===================================================================
 * RECURSOS GLOBAIS
 * ===================================================================
 */
typedef struct {
    char ssid[SSID_MAX_LENGTH + 1];
} wifi_ssid_msg_t;

static QueueHandle_t queue_ssid_data = NULL;
static SemaphoreHandle_t mutex_secure_list = NULL;
static EventGroupHandle_t wifi_event_group = NULL;

static const char *secure_ssid_list[SECURE_LIST_SIZE] = {
    "RedeSegura_1",
    "WiFi_Corporativo",
    "MinhaCasa_5G",
    "ESP_LAB_SEG",
    "saf"
};

/*
 * ===================================================================
 * HANDLER DE EVENTOS WI-FI
 * ===================================================================
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf(LOG_PREFIX " [WIFI] STA iniciada, tentando conectar...\n");
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf(LOG_PREFIX " [WIFI][WARN] STA desconectada; sinalizando falha.\n");
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        printf(LOG_PREFIX " [WIFI] IP obtido: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/*
 * ===================================================================
 * INICIALIZACAO DE WIFI
 * ===================================================================
 */
static void wifi_init_sta(void)
{
    printf(LOG_PREFIX " [WIFI] Inicializando Wi-Fi em modo STA...\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf(LOG_PREFIX " [WIFI] Wi-Fi STA iniciado.\n");
}

/*
 * ===================================================================
 * TASK: MONITOR DE WIFI (Produtor da fila)
 * ===================================================================
 */
static void tarefa_monitor_wifi(void *pvParameters)
{
    printf(LOG_PREFIX " [MONITOR] Task iniciada, aguardando conexao.\n");
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        printf(LOG_PREFIX " [MONITOR][ERRO] Falha ao conectar no Wi-Fi. Reiniciando...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    wifi_ap_record_t ap_info;
    wifi_ssid_msg_t data_to_send;

    while (1) {
        memset(&ap_info, 0, sizeof(ap_info));
        memset(&data_to_send, 0, sizeof(data_to_send));

        esp_err_t ap_status = esp_wifi_sta_get_ap_info(&ap_info);
        if (ap_status == ESP_OK) {
            memcpy(data_to_send.ssid, ap_info.ssid, SSID_MAX_LENGTH);

            bool sent = false;
            for (int attempt = 0; attempt < MAX_QUEUE_SEND_RETRIES; attempt++) {
                if (xQueueSend(queue_ssid_data, &data_to_send,
                               pdMS_TO_TICKS(QUEUE_SEND_TIMEOUT_MS)) == pdPASS) {
                    printf(LOG_PREFIX " [FILA] Dado enviado com sucesso! SSID: %s\n", data_to_send.ssid);
                    printf(LOG_PREFIX " [MONITOR] SSID '%s' enviado para validacao.\n", data_to_send.ssid);
                    sent = true;
                    break;
                }
                printf(LOG_PREFIX " [MONITOR][WARN] Fila cheia (tentativa %d/%d).\n", attempt + 1, MAX_QUEUE_SEND_RETRIES);
            }

            if (!sent) {
                printf(LOG_PREFIX " [MONITOR][ERRO] Fila sem consumo. Reiniciando para recuperar.\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            printf(LOG_PREFIX " [MONITOR][WARN] Nao foi possivel obter o AP atual (%s).\n", esp_err_to_name(ap_status));
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_DELAY_MS));
    }
}

/*
 * ===================================================================
 * TASK: VERIFICADOR DE SEGURANÇA (Consumidor da fila)
 * ===================================================================
 */
static void tarefa_verificador_seguro(void *pvParameters)
{
    printf(LOG_PREFIX " [VERIFICADOR] Task iniciada, aguardando SSIDs.\n");
    wifi_ssid_msg_t received_data;
    int timeout_counter = 0;

    while (1) {
        if (xQueueReceive(queue_ssid_data, &received_data,
                          pdMS_TO_TICKS(CHECKER_QUEUE_TIMEOUT_MS)) == pdPASS) {
            timeout_counter = 0;

            if (xSemaphoreTake(mutex_secure_list, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT_MS)) == pdTRUE) {
                bool is_secure = false;
                for (int i = 0; i < SECURE_LIST_SIZE; i++) {
                    if (strcmp(received_data.ssid, secure_ssid_list[i]) == 0) {
                        is_secure = true;
                        break;
                    }
                }
                xSemaphoreGive(mutex_secure_list);

                if (!is_secure) {
                    printf(LOG_PREFIX " [ALERTA_SEGURANCA] Rede nao autorizada detectada (%s).\n", received_data.ssid);
                    printf(LOG_PREFIX " [ALERTA_SEGURANCA] SSID NAO AUTORIZADO: %s\n", received_data.ssid);
                } else {
                    printf(LOG_PREFIX " [VERIFICADOR] Rede '%s' validada como segura.\n", received_data.ssid);
                }
            } else {
                printf(LOG_PREFIX " [VERIFICADOR][ERRO] Semaforo da lista indisponivel. Reiniciando...\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            timeout_counter++;

            if (timeout_counter == CHECKER_TIMEOUT_WARN_LIMIT) {
                printf(LOG_PREFIX " [VERIFICADOR][WARN] Timeout nivel 1: nenhum SSID recebido.\n");
            } else if (timeout_counter >= CHECKER_TIMEOUT_RESET_LIMIT) {
                printf(LOG_PREFIX " [VERIFICADOR][ERRO] Timeout critico: monitor possivelmente travado. Reiniciando sistema.\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            } else if (timeout_counter > CHECKER_TIMEOUT_WARN_LIMIT) {
                printf(LOG_PREFIX " [VERIFICADOR][WARN] Timeout nivel 2: aguardando atividade (%d).\n", timeout_counter);
            }
        }

        esp_task_wdt_reset();
    }
}

/*
 * ===================================================================
 * TASK: SUPERVISOR (Heartbeat e métricas)
 * ===================================================================
 */
static void tarefa_supervisora(void *pvParameters)
{
    printf(LOG_PREFIX " [SUPERVISOR] Task ativa.\n");
    esp_chip_info_t chip_info;

    while (1) {
        esp_chip_info(&chip_info);
         printf(LOG_PREFIX " [SUPERVISOR] Heartbeat -> Cores:%d Rev:%d Heap:%" PRIu32 " bytes\n",
             chip_info.cores,
             chip_info.revision,
             esp_get_free_heap_size());

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_DELAY_MS));
    }
}

/*
 * ===================================================================
 * FUNCAO PRINCIPAL
 * ===================================================================
 */
void app_main(void)
{
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };

    esp_err_t wdt_status = esp_task_wdt_init(&wdt_config);
    if (wdt_status == ESP_ERR_INVALID_STATE) {
        printf(LOG_PREFIX " [WDT][WARN] TWDT ja inicializado pelo sistema.\n");
    } else if (wdt_status != ESP_OK) {
        printf(LOG_PREFIX " [WDT][ERRO] Falha ao inicializar TWDT (%s). Reiniciando.\n", esp_err_to_name(wdt_status));
        esp_restart();
    }

    queue_ssid_data = xQueueCreate(QUEUE_LENGTH, sizeof(wifi_ssid_msg_t));
    mutex_secure_list = xSemaphoreCreateMutex();
    wifi_event_group = xEventGroupCreate();

    if (queue_ssid_data == NULL || mutex_secure_list == NULL || wifi_event_group == NULL) {
        printf(LOG_PREFIX " [MAIN][ERRO] Falha ao criar recursos FreeRTOS. Reiniciando...\n");
        esp_restart();
    }

    wifi_init_sta();

    TaskHandle_t h_monitor = NULL;
    TaskHandle_t h_checker = NULL;
    TaskHandle_t h_supervisor = NULL;

    if (xTaskCreate(tarefa_monitor_wifi, "tarefa_monitor_wifi", 4096, NULL,
                    tskIDLE_PRIORITY + 3, &h_monitor) != pdPASS) {
        printf(LOG_PREFIX " [MAIN][ERRO] Falha ao criar tarefa_monitor_wifi. Reiniciando...\n");
        esp_restart();
    }

    if (xTaskCreate(tarefa_verificador_seguro, "tarefa_verificador_seguro", 4096, NULL,
                    tskIDLE_PRIORITY + 2, &h_checker) != pdPASS) {
        printf(LOG_PREFIX " [MAIN][ERRO] Falha ao criar tarefa_verificador_seguro. Reiniciando...\n");
        esp_restart();
    }

    if (xTaskCreate(tarefa_supervisora, "tarefa_supervisora", 3072, NULL,
                    tskIDLE_PRIORITY + 1, &h_supervisor) != pdPASS) {
        printf(LOG_PREFIX " [MAIN][ERRO] Falha ao criar tarefa_supervisora. Reiniciando...\n");
        esp_restart();
    }

    ESP_ERROR_CHECK(esp_task_wdt_add(h_monitor));
    ESP_ERROR_CHECK(esp_task_wdt_add(h_checker));
    ESP_ERROR_CHECK(esp_task_wdt_add(h_supervisor));

    printf(LOG_PREFIX " [MAIN] Sistema inicializado com sucesso. Monitoramento ativo.\n");
}