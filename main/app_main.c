/* MQTT Mutual Authentication Example */

#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "wifi/wifi.h"
#include "dht11/dht11.h"


#define DHT_GPIO 21
#define BROKER_URI "mqtts://broker.emqx.io:8883"
#define MQTT_PUB_DATA_DHT "esp32/dht/data"

#define MQTT_PUB_STATUS_DATA_ACTIVE_TRUE "{\"active\":\"true\"}"
#define MQTT_PUB_STATUS_DATA_ACTIVE_FALSE "{\"active\":\"false\"}"

char mqtt_pub_status_topic[200];
static const char *TAG = "MQTTS";
uint32_t MQTT_CONNECTED = 0;
uint32_t ACTIVE = 0;
static esp_mqtt_client_handle_t client;
char mac_id[50];
char device_id[300];

extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_client_crt_end");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t client_key_pem_end[] asm("_binary_client_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_broker_CA_crt_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_broker_CA_crt_end");

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void handle_event(const char *topic, const int topic_len, const char *data, const int data_len){

    char my_topic[topic_len+1];
    char format[] = "%.*s";
    int max_len = sizeof my_topic;
    snprintf(my_topic, max_len, format, topic_len, topic);

    char my_data[data_len+1];
    char format2[] = "%.*s";
    int max_len2 = sizeof my_data;
    snprintf(my_data, max_len2, format2, data_len, data);
    

    if (strcmp (my_topic, mqtt_pub_status_topic) == 0){
        if (strcmp(my_data, MQTT_PUB_STATUS_DATA_ACTIVE_TRUE) == 0){
            ESP_LOGI(TAG, "Turning ON Sensor");
            ACTIVE = 1;
        }else if (strcmp(my_data, MQTT_PUB_STATUS_DATA_ACTIVE_FALSE) == 0){
            ESP_LOGI(TAG, "Turning OFF Sensor");
            ACTIVE = 0;
        }else{
            ESP_LOGI(TAG, "Not recognized status");
            puts(my_data);
        }
    } else {
        ESP_LOGI(TAG, "Not recognized topic");
        puts(my_topic);
    }

}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        MQTT_CONNECTED = 1;
        ESP_LOGI(TAG, "Connected to broker");
        msg_id = esp_mqtt_client_subscribe(client, mqtt_pub_status_topic, 0);
        ESP_LOGI(TAG, "Subscribed successful, msg_id=%d", msg_id);
        int pub_msg_id = esp_mqtt_client_publish(client, mqtt_pub_status_topic, "ask_status", 0, 0, 0);
        ESP_LOGI(TAG, "Published successful, pub_msg_id=%d", pub_msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        MQTT_CONNECTED = 0;
        ESP_LOGI(TAG, "Disconnected from broker");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        handle_event(event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URI,
        .client_cert_pem = (const char *)client_cert_pem_start,
        .client_key_pem = (const char *)client_key_pem_start,
        .cert_pem = (const char *)server_cert_pem_start,
    };

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}


void dht_task(void *pvParameter)
{
    DHT11_init(DHT_GPIO);
	printf( "Starting DHT Task\n\n");

	while(1) {

        if (ACTIVE) {

            int temp = DHT11_read().temperature;
            int hum = DHT11_read().humidity;

            char hum_value[12];
            sprintf(hum_value, "%d", hum);
            char temp_value[12];
            sprintf(temp_value, "%d", temp);

            printf("=== Reading DHT ===\n" );
            printf("Temperature is %d \n",temp);
            printf("Humidity is %d\n", hum);
            printf("connected is %d\n", MQTT_CONNECTED);

            if (MQTT_CONNECTED) {
                char json[300];
                char format[] = "{\"device_id\":\"%s\", \"temp\":\"%s\", \"hum\":\"%s\"}";
                int max_len = sizeof json;
                snprintf(json, max_len, format, device_id, temp_value, hum_value);

                esp_mqtt_client_publish(client, MQTT_PUB_DATA_DHT, json, 0, 0, 0);
            }
            vTaskDelay( 30000 / portTICK_RATE_MS );
	    }
    }
}

void get_mac_id(void){
    uint8_t mac_address[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    esp_err_t ret = ESP_OK;
    uint8_t base_mac_addr[6];
    ret = esp_efuse_mac_get_default(base_mac_addr);
    if(ret != ESP_OK){
            ESP_LOGE(TAG, "Failed to get base MAC address from EFUSE BLK0. (%s)", esp_err_to_name(ret));
            ESP_LOGE(TAG, "Aborting");
            abort();
    }
    uint8_t index = 0;

    for (uint8_t i=0; i<6; i++) {
        index += sprintf(&mac_id[index], "%02x", base_mac_addr[i]);
    }
    ESP_LOGI(TAG, "mac_id = %s", mac_id);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    wifi_connect();

    get_mac_id();

    char device_id_format[] = "device_id_%s";
    int device_id_max_len = sizeof device_id;
    snprintf(device_id, device_id_max_len, device_id_format, mac_id);


    char mqtt_pub_status_topic_format[] = "esp32/status/%s";
    int mqtt_pub_status_topic_max_len = sizeof mqtt_pub_status_topic;
    snprintf(mqtt_pub_status_topic, mqtt_pub_status_topic_max_len, mqtt_pub_status_topic_format, device_id);

    mqtt_app_start();

    xTaskCreate(&dht_task, "dht_task", 2048, NULL, 5, NULL );
}
