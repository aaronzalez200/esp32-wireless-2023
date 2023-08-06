#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/gpio.h"

#include "nvs_flash.h"
#include "esp_http_client.h"
#include "wifi_app.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "math.h"
#include "get_adc.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "driver/uart.h"

#include "mbedtls/sha256.h"
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

extern const uint8_t certificate_pem_start[] asm("_binary_certificate_pem_start");
extern const uint8_t certificate_pem_end[] asm("_binary_certiciate_pem_end");
extern const uint8_t mqtt_cert_pem_start[] asm("_binary_mqtt_cert_pem_start");
extern const uint8_t mqtt_cert_pem_end[] asm("_binary_mqtt_cert_pem_end");


#define DEFAULT_VREF    3300        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64          //Multisampling
#define TXD_PIN GPIO_NUM_17         //UART TX pin
#define RXD_PIN GPIO_NUM_16         //UART RX pin
#define UART UART_NUM_2             //UART NUM2 used

// UART
static const int RX_BUF_SIZE = 1024;

// GET request retry
int retryGetRequest = 0;

int num = 0;

// Declare the mqttClient variable as a global variable
esp_mqtt_client_handle_t mqttClient;

// Declare the HTTP handlers as a global variable
esp_http_client_handle_t weatherClient; // GET weather
esp_http_client_handle_t client;        // GET
esp_http_client_handle_t client2;       // PUT
esp_http_client_handle_t client3;       // POST

// Define variables to store the values of "Supply" and "Heat"
int supply_value = 0;                                           // supply value to send over HTTP
int heat_value = 0;                                             // heat value to send over HTTP
int emc_value = 0;                                              // emc enable value to send over HTTP
int emc_toggle_value = 0;                                       // emc heater on/off value to send over HTTP
int temp_limit_value = 0;                                       // temp limit value to send over HTTP
int snow_value = 0;                                             // snow value to send over HTTP
int gfep_value = 0;                                             // gfep value to send over HTTP
int temp_value = 0;                                             // temp value to send over HTTP

// Variables that handles input readings
volatile int supplyLastState = -1;
volatile int heatLastState = -1;
volatile int emcLastState = -1;
volatile int emctoggleLastState = -1;
volatile int temp_limitLastState = -1;
volatile int snowLastState = -1;
volatile int gfepLastState = -1;
volatile int temp_readingLastState = -1;

// Variables for WiFi operation
extern bool wifi_connected;         //Flag to read if connected to internet
const int MAX_RETRY_COUNT = 10;     // Maximum number of retries
const int RETRY_DELAY = 1000;       // Delay between retries (in milliseconds)
int retry_count = 0;                // global retry int
int temperature;                    // store temp
int check_post = 0;                 // check if device is registered in the Database already

// Variables for Serial Number & Security Key for Serial Number registration
uint8_t mac_buffer[6];                                          // mac address of ESP32; gets converted to a serial number
uint8_t baseMac[6];
uint64_t serial_number = 0;                                     // used to convert MAC address to serial number for HTTP requests
unsigned char hash[4];                                          // used for hashing serial number to generate security key
char security_key[21];  // 20 characters + null terminator

// Assigning variables for I/O pins
gpio_num_t Supply = GPIO_NUM_18;                                // GPIO 18 - Supply
gpio_num_t Heat = GPIO_NUM_5;                                   // GPIO 05 - Heat
gpio_num_t EMC = GPIO_NUM_19;                                   // GPIO 19 - EMC
gpio_num_t GFEP = GPIO_NUM_21;                                  // GPIO 21 - GFEP
gpio_num_t Temp_Limit = GPIO_NUM_22;                            // GPIO 22 - Temp_Limit
gpio_num_t Snow = GPIO_NUM_23;                                  // GPIO 23 - Snow
gpio_num_t EMC_ON = GPIO_NUM_32;                                // GPIO 32 - EMC_ON
gpio_num_t EMC_OFF = GPIO_NUM_33;                               // GPIO 33 - EMC_OFF

// Declare the url string and a buffer to hold the concatenated string
char url[100] = "https://networketicloud.com/api/devices?serial=";  // Website API for getting user's device data 
char serial_str[20];                                                // Serial number used in API requests, unique to each ESP32

static const char *TAG2 = "BLE";

// Handle BLE for WiFi provisioning 
static void event_handler_wifi(void* arg, esp_event_base_t event_base, int event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG2, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG2, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG2, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG2, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    }
}

static void startBLE()
{
    // Adding BLE for WiFi provisioning
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT,
    };
    const char *service_name = "my_device";
    const char *service_key  = "password";

    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
    const char *pop = "abcd1234";

    ESP_ERROR_CHECK( wifi_prov_mgr_init(config) );
    ESP_ERROR_CHECK( wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key) );
   
}

// Handle HTTP PUT requests
esp_err_t client_event_put_handler(esp_http_client_event_t *evt)    // Handles HTTP PUT requests
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Process received data in evt->data
            printf("Processing data\n");
            break;
        case HTTP_EVENT_ON_FINISH:
            // Request completed
            printf("Request completed\n");
            break;
        case HTTP_EVENT_ERROR:
            // Request error occurred
            printf("Processing/Request error\n");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Handle HTTP POST requests
esp_err_t client_event_post_handler(esp_http_client_event_t *evt)    // Handles HTTP POST requests
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Process received data in evt->data
            printf("Processing data\n");
            break;
        case HTTP_EVENT_ON_FINISH:
            // Request completed
            printf("Request completed\n");
            break;
        case HTTP_EVENT_ERROR:
            // Request error occurred
            printf("Processing/Request error\n");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Handle HTTP GET requests
esp_err_t client_event_get_handler(esp_http_client_event_handle_t evt)  // Handles HTTP GET requests
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
        {
            retryGetRequest = 1;
            printf("Good GET Request???\n");
            printf("retryGetRequest set to 1...: %d\n", retryGetRequest);
             esp_http_client_handle_t client = evt->client;  // Get the client handle from the event
            // Check if device exists in the database
            if (memcmp(evt->data, "{\"devices\":[]}", 14) == 0) {
                printf("No data found\n");
                check_post = 1;                 // No data found, set variable to POST to register device to database
                break;
            } else {
                printf("Data found\n");
                check_post = 0;                 // Data found, set variable to skip POST to database, already exists
            }
            fwrite(evt->data, evt->data_len, 1, stdout);        // Print data recieved from GET request
            cJSON *root = cJSON_Parse((char *)evt->data);       // Parse JSON
            if (root == NULL) {
                printf("\nError parsing JSON: %s\n", cJSON_GetErrorPtr());
                break;
            }
            cJSON *devices_array = cJSON_GetObjectItemCaseSensitive(root, "devices");
            if (!cJSON_IsArray(devices_array)) {
                printf("Error: devices is not an array\n");
                cJSON_Delete(root);
                break;
            }
            // Extract the first device object from the devices array
            cJSON *device_obj = cJSON_GetArrayItem(devices_array, 0);
            cJSON *supply_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Supply");
            cJSON *heat_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Heat");
            cJSON *emc_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC");
            cJSON *snow_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Snow");
            cJSON *temp_limit_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Temp_Limit");
            cJSON *gfep_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "GFEP");
            cJSON *emc_toggle_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC_Toggle");

            // Check if Object
            if (!cJSON_IsObject(device_obj)) {
                printf("Error: device is not an object\n");
                cJSON_Delete(root);
                break;
            }
            
            // Extract the values from the JSON object, such as Supply, Heat, Alarm, etc....
            if (!cJSON_IsNumber(supply_value_obj)) {
                printf("Error: Supply value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            
            if (!cJSON_IsNumber(heat_value_obj)) {
                printf("Error: Heat value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            
            if (!cJSON_IsNumber(emc_value_obj)) {
                printf("Error: EMC value is not a number\n");
                cJSON_Delete(root);
                break;
            }  

            if (!cJSON_IsNumber(snow_value_obj)) {
                printf("Error: Snow value is not a number\n");
                cJSON_Delete(root);
                break;
            }
           
            if (!cJSON_IsNumber(temp_limit_value_obj)) {
                printf("Error: Temp Limit value is not a number\n");
                cJSON_Delete(root);
                break;
            }

            if (!cJSON_IsNumber(gfep_value_obj)) {
                printf("Error: GFEP value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            
            if (!cJSON_IsNumber(emc_toggle_value_obj)) {
                printf("Error: EMC_Toggle value is not a number\n");
                cJSON_Delete(root);
                break;
            }

            // Set object values recieved to xxxx_value variables
            supply_value = supply_value_obj->valueint;
            heat_value = heat_value_obj->valueint;
            emc_value = emc_value_obj->valueint;
            snow_value = snow_value_obj->valueint;
            temp_limit_value = temp_limit_value_obj->valueint;
            gfep_value = gfep_value_obj->valueint;
            emc_toggle_value = emc_toggle_value_obj->valueint;
        
            // Print values
            printf("Supply value: %d\n", supply_value);
            printf("Heat value: %d\n", heat_value);
            printf("EMC value: %d\n", emc_value);
            printf("EMC_Toggle value: %d\n", emc_toggle_value);
            printf("Snow value: %d\n", snow_value);
            printf("GFEP value: %d\n", gfep_value);
            printf("Temp Limit value: %d\n", temp_limit_value);
            printf("=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");

            // Cleanup cJSON memory
            cJSON_Delete(root);    
            break;
        }
        case HTTP_EVENT_ERROR:
            printf("HTTP_EVENT_ERROR when performing GET request\n");
            retryGetRequest = 0;
            printf("retryGetRequest: %d\n", retryGetRequest);
            break;
        case HTTP_EVENT_DISCONNECTED:
            printf("HTTP_EVENT_DISCONNECTED\n");
            // Handle the disconnection event
            printf("retryGetRequest: %d\n", retryGetRequest);
            // Return an appropriate error code or take necessary actions
            break;
        default:
            break;
    }
	return ESP_OK;
}

// Handle HTTP GET requests
esp_err_t client_event_get_weather_handler(esp_http_client_event_handle_t evt)  // Handles HTTP GET requests
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
        {
            retryGetRequest = 1;
            printf("Processing Weather Request...\n");
            printf("retryGetRequest set to 1...: %d\n", retryGetRequest);
             esp_http_client_handle_t client = evt->client;  // Get the client handle from the event
            // Check if device exists in the database
            fwrite(evt->data, evt->data_len, 1, stdout);        // Print data recieved from GET request
            cJSON *root = cJSON_Parse((char *)evt->data);       // Parse JSON
            if (root == NULL) {
                printf("\nError parsing JSON: %s\n", cJSON_GetErrorPtr());
                break;
            }
            // Cleanup cJSON memory
            cJSON_Delete(root);    
            break;
        }
        case HTTP_EVENT_ERROR:
            printf("HTTP_EVENT_ERROR when performing weather GET request\n");
            retryGetRequest = 0;
            printf("retryGetRequest: %d\n", retryGetRequest);
            break;
        case HTTP_EVENT_DISCONNECTED:
            printf("HTTP_EVENT_DISCONNECTED\n");
            // Handle the disconnection event
            printf("retryGetRequest: %d\n", retryGetRequest);
            // Return an appropriate error code or take necessary actions
            break;
        default:
            break;
    }
	return ESP_OK;
}

esp_http_client_config_t config_get = {         // GET request config
    .url = url,
    .method = HTTP_METHOD_GET,
    .event_handler = client_event_get_handler,
    .auth_type = HTTP_AUTH_TYPE_BASIC,
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    .buffer_size = 1024*4,
    .cert_pem = (const char *)certificate_pem_start,
};
esp_http_client_config_t config_put = {        // PUT request config
    .url = url,
    .method = HTTP_METHOD_PUT,
    .event_handler = client_event_put_handler,
    .auth_type = HTTP_AUTH_TYPE_BASIC,
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    .cert_pem = (const char *)certificate_pem_start,
};
esp_http_client_config_t config_post = {       // POST request config
    .url = url,
    .method = HTTP_METHOD_POST,
    .event_handler = client_event_post_handler,
    .auth_type = HTTP_AUTH_TYPE_BASIC,
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    .buffer_size = 1024,
    .cert_pem = (const char *)certificate_pem_start,
};

esp_http_client_config_t config_get_weather = {         // GET request config
    .url = "https://api.weatherapi.com/v1/current.json?key=65d8e52d2f9442c0bf7142919231707&q=South_Bend&aqi=no",
    .method = HTTP_METHOD_GET,
    .event_handler = client_event_get_weather_handler,
    .auth_type = HTTP_AUTH_TYPE_NONE,
    .transport_type = HTTP_TRANSPORT_OVER_TCP,
    .buffer_size = 1024*4,
};

// Configure I/O pins
void config_IOs() 
{
    gpio_set_direction(EMC_ON, GPIO_MODE_OUTPUT);       // Output pins - EMC_ON
    gpio_set_direction(EMC_OFF, GPIO_MODE_OUTPUT);      // Output pins - EMC_OFF
    gpio_set_direction(EMC, GPIO_MODE_OUTPUT);          // Output pins - EMC_Enable
    gpio_set_direction(Supply, GPIO_MODE_INPUT);        // Input pins - Supply
    gpio_pullup_en(Supply);                             // Enable pull-up for Supply
    gpio_set_direction(Heat, GPIO_MODE_INPUT);          // Input pins - Heat
    gpio_pullup_en(Heat);                               // Enable pull-up for Heat
    gpio_set_direction(GFEP, GPIO_MODE_INPUT);          // Input pins - GFEP
    gpio_pullup_en(GFEP);                               // Enable pull-up for GFEP
    gpio_set_direction(Temp_Limit, GPIO_MODE_INPUT);    // Input pins - Temp_Limit
    gpio_pullup_en(Temp_Limit);                         // Enable pull-up for Temp_Limit
    gpio_set_direction(Snow, GPIO_MODE_INPUT);          // Input pins - Snow
    gpio_pullup_en(Snow);                               // Enable pull-up for Snow
}

static void tx_task(void *arg) {
  int num = 1;
  char Txdata[100];
  while (1) {
    sprintf(Txdata, "%d\r\n", num);
    uart_write_bytes(UART, Txdata, strlen(Txdata));

    num = (num % 4) + 1;  // Increment num and wrap around to 1 after 4

    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

static void rx_task(void *arg)
{
    static const char *RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE+1);
    while (1) {
        const int rxBytes = uart_read_bytes(UART, data, RX_BUF_SIZE, 500 / portTICK_PERIOD_MS);
        if (rxBytes > 0) {
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);
        }
    }
    free(data);
}

void hashString(const char* input, size_t inputLength, unsigned char* output) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char*)input, inputLength);
    mbedtls_sha256_finish(&ctx, output);
    mbedtls_sha256_free(&ctx);
}

void printHash(const unsigned char* hash, size_t hashLength) {
    for (size_t i = 0; i < hashLength; i++) {
        printf("%02X", hash[i]);
    }
    printf("\n");
}

void hashToString(const unsigned char* hash, size_t hashLength, char* buffer) {
    for (size_t i = 0; i < hashLength; i++) {
        sprintf(buffer + (i * 2), "%02X", hash[i]);
    }
    buffer[hashLength * 2] = '\0'; // Add null terminator at the end
}

static const char *TAG = "MQTT_EXAMPLE";
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t mqttClient = event->client;
    int msg_id;
    char topic[64];  // Assuming a maximum topic length of 64 characters
    snprintf(topic, sizeof(topic), "devices/%s", serial_str);

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(mqttClient, topic, "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(mqttClient, topic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(mqttClient, topic, "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        if (strncmp(event->data, "web client connected", event->data_len) == 0) {
            printf("Web client has connected! Time to read IOs and make PUT request\n");
        }
        if (strncmp(event->data, "emc_value: 1", event->data_len) == 0) {
            printf("Toggle EMC OFF; Don't accept Manual Heat ON or OFF\n");
            emc_value = 1;
            // Turn off override
            gpio_set_level(EMC_ON, 1); //EMC ON so set override ON HIGH
            gpio_set_level(EMC_OFF, 1); //EMC ON so set override OFF HIGH
            gpio_set_level(EMC, 1);
        }
        if (strncmp(event->data, "emc_value: 0", event->data_len) == 0) {
            printf("Toggle EMC ON; Accept Manual Heat ON or OFF\n");
            emc_value = 0;   
            gpio_set_level(EMC_ON, 0); //EMC ON so set override ON HIGH
            gpio_set_level(EMC_OFF, 0); //EMC ON so set override OFF HIGH
            gpio_set_level(EMC, 0);
        }
        if (strncmp(event->data, "emc_toggle: 1", event->data_len) == 0) {
             // If Override ON is selected & EMC is enabled
            if (emc_value == 0) {
            // printf("EMC enabled, EMC Override ON\n");
            gpio_set_level(EMC_ON, 0); //EMC ON so set override ON LOW
            gpio_set_level(EMC_OFF, 1); //EMC ON so set override OFF HIGH
            }
        }
        if (strncmp(event->data, "emc_toggle: 0", event->data_len) == 0) {
            // If Override OFF is selected & EMC is enabled
            if (emc_value == 0) {
             // printf("EMC enabled, EMC Override OFF\n");
            gpio_set_level(EMC_ON, 1); //EMC ON so set override ON LOW
            gpio_set_level(EMC_OFF, 0); //EMC ON so set override OFF HIGH
            }
        }
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
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = "wss://kbfd9c01.ala.us-east-1.emqxsl.com:8084/mqtt",
            },
            .verification = {
                .certificate = (const char *)mqtt_cert_pem_start
            },
        },
        .credentials = {
            .username = "ESP32ETI",
            .authentication.password = "ESP32ETI",
        }
    };
    printf("Config MQTT\n");
    mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    printf("Config MQTT DONE\n");
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    printf("Register mqtt event\n");
    esp_mqtt_client_register_event(mqttClient, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    printf("Register mqtt event DONE\n");
    printf("START MQTT client\n");
    esp_mqtt_client_start(mqttClient);
    printf("START MQTT client STARTED\n");
}

// Make HTTP requests to database with ESP32
static void get_database_info()
{
    client = esp_http_client_init(&config_get);        // Initalize GET request

    // Add API Key to header
    esp_http_client_set_header(client, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 

    vTaskDelay(300 / portTICK_PERIOD_MS);       // wait 100mS

    printf("Making GET request.... \n");
    esp_http_client_perform(client);            // makes GET request from database filtering by its serial number under devices

    printf("\nGET request done.... \n");
    vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
    esp_http_client_cleanup(client);       // clean GET request
    vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
    while (retryGetRequest == 0) {
        if (retryGetRequest == 1) {
            printf("Breaking\n");
            break;
        }
        for (num = 0; num <= 10; num++) {
            vTaskDelay(100 / portTICK_PERIOD_MS);                   // wait 100mS
            client = esp_http_client_init(&config_get);        // Initalize GET request

            // Add API Key to header
            esp_http_client_set_header(client, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 

            vTaskDelay(300 / portTICK_PERIOD_MS);       // wait 100mS
            esp_http_client_perform(client);   // makes GET request from database filtering by its serial number under devices
            printf("Made GET request again...\n");
            vTaskDelay(100 / portTICK_PERIOD_MS);                   // wait 100mS
            esp_http_client_cleanup(client);                   // clean GET request
            if (num == 10) {
                printf("Maximum retry count reached. Giving up.\n");
                // Reboot the ESP32
                printf("Restarting ESP32...\n");
                esp_restart();
            }
            printf("Num: %d\n", num);
            if (retryGetRequest == 1) {
                break;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);                   // wait 100mS
        }
    }
    
    printf("Successful GET REQUEST! \n");

    if (check_post == 1) {
        // Create a JSON payload string with the serial number value
        printf("Time to register devices... currently does not exist in the database...");
        char post_data[250];
        snprintf(post_data, sizeof(post_data), "{\"serial\": \"%s\", \"security_key\": \"%s\", \"Supply\": 0, \"EMC\": 0, \"Temp_Limit\": 0, \"Heat\": 0, \"Snow\": 0, \"GFEP\": 0, \"EMC_Toggle\": 0, \"location\": \"South Bend, Indiana, United States of America\", \"location_enable\": 0}", serial_str, security_key);
        printf("Payload: %s\n", post_data);
        char url[250];
        snprintf(url, sizeof(url), "https://networketicloud.com/api/devices");
        client3 = esp_http_client_init(&config_post);       // Initalize POST request
        vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
        esp_http_client_set_header(client3, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
        esp_http_client_set_header(client3, "Content-Type", "application/json");
        esp_http_client_set_post_field(client3, post_data, strlen(post_data));           // Set the body of the POST request
        esp_err_t err = esp_http_client_perform(client3);            // makes POST request

         if (err != ESP_OK) {
            printf("POST request failed: %s\n", esp_err_to_name(err));
            // Handle the error or take appropriate action
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
        esp_http_client_cleanup(client3);            // clean up 1st HTTP request; avoid memory leaks
    }

    // Logic below; Reads from database and acts accordingly
    // Heat logic
    if (gpio_get_level(Heat) == 1) {
        // printf("Heat is low\n");
        heat_value = 1;
    } else {
        // printf("Heat is high\n");
        heat_value = 0;
    }

    // Supply logic
    if (gpio_get_level(Supply) == 1) {
        // printf("Supply is low\n");
        supply_value = 1;
    } else {
        // printf("Supply is high\n");
        supply_value = 0;
    }

    // EMC logic
    if (emc_value == 1) {
        // printf("EMC is low\n");
        gpio_set_level(EMC, 0);
        // Turn off override
        gpio_set_level(EMC_ON, 1); //EMC ON so set override ON HIGH
        gpio_set_level(EMC_OFF, 1); //EMC ON so set override OFF HIGH
    } else {
        // printf("EMC is high\n");
        gpio_set_level(EMC, 1);
        // If Override ON is selected & EMC is enabled
        if (emc_toggle_value == 1) {
            // printf("EMC enabled, EMC Override ON\n");
            gpio_set_level(EMC_ON, 0); //EMC ON so set override ON LOW
            gpio_set_level(EMC_OFF, 1); //EMC ON so set override OFF HIGH
            // If Override OFF is selected & EMC is enabled
        } else {
            // printf("EMC enabled, EMC Override OFF\n");
            gpio_set_level(EMC_ON, 1); //EMC ON so set override ON LOW
            gpio_set_level(EMC_OFF, 0); //EMC ON so set override OFF HIGH
        }
    } 

    // Snow logic
    if (gpio_get_level(Snow) == 1) {
        // printf("Snow is low \n");
        snow_value = 1;
    } else {
        // printf("Snow is high\n");
        snow_value = 0;
    }

    // GFEP logic
    if (gpio_get_level(GFEP) == 1) {
        // printf("GFEP is low\n");
        gfep_value = 1;
    } else {
        // printf("GFEP is high\n");
        gfep_value = 0;
    }

    // Temp Limit logic
    if (gpio_get_level(Temp_Limit) == 1) {
        // printf("Temp Limit is low\n");
        temp_limit_value = 1;
    } else {
        // printf("Temp Limit is high\n");
        temp_limit_value = 0;
    }

    // printf("serial: %s, Supply: %i, EMC: %i, EMC_Toggle: %i, Temp_Limit: %i, Heat: %i, Snow: %i, GFEP: %i", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value);
    // Here is where we get the unique MAC ID of the ESP32 Chip 
    esp_wifi_get_mac(WIFI_IF_STA, mac_buffer);          // Get MAC Addres of STA interface. [STA/AP/NAN/MAX] We're using STA
    printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac_buffer[0], mac_buffer[1], mac_buffer[2], mac_buffer[3], mac_buffer[4], mac_buffer[5]);
    printf("Making PUT Request...\n");

    client2 = esp_http_client_init(&config_put);       // Initalize PUT request

    // Send the PUT request
    char dataDEVICE[200];
    snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
    int data_len = strlen(dataDEVICE);
    char content_length[16];
    snprintf(content_length, sizeof(content_length), "%d", data_len);
    esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
    esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
    esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
    esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
    printf("%s", dataDEVICE);
    esp_http_client_perform(client2);                                               // Peform PUT request
    vTaskDelay(3000 / portTICK_PERIOD_MS);                                          // Wait 3 seconds
    esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks

    // Read ADC
    read_and_print_adc_value(&temperature);
}

void publish_mqtt_message(esp_mqtt_client_handle_t mqttClient, const char *serial_str, const char *message)
{
    char topic[64];  // Assuming a maximum topic length of 64 characters
    snprintf(topic, sizeof(topic), "devices/%s", serial_str);
    int msg_id = esp_mqtt_client_publish(mqttClient, topic, message, 0, 0, 0 );
    ESP_LOGI(TAG, "Published message ID: %d", msg_id);
}

// Task that reads IOs to send to database & MQTT broker
// Task that reads IOs to send to database & MQTT broker
static void ios_task(void)
{
    printf("\nStarting task...\n");

    // Perform PUT request
    char put_url[200];
    snprintf(put_url, sizeof(put_url), "https://networketicloud.com/api/devices");

    esp_http_client_config_t config_put = {
        .url = put_url,
        .method = HTTP_METHOD_PUT,
        .event_handler = client_event_put_handler,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .cert_pem = (const char *)certificate_pem_start,
    };

    int supplyLastState = -1;
    int heatLastState = -1;
    int emcLastState = -1;
    int tempLimitLastState = -1;
    int snowLastState = -1;
    int gfepLastState = -1;

    // Read the input states
    int supplyState = gpio_get_level(Supply);
    int heatState = gpio_get_level(Heat);
    int emcState = gpio_get_level(EMC);
    int tempLimitState = gpio_get_level(Temp_Limit);
    int snowState = gpio_get_level(Snow);
    int gfepState = gpio_get_level(GFEP);

    const char* supplyMessage = (supplyState == 0) ? "sup_on" : "sup_off";
    const char* heatMessage = (heatState == 0) ? "heat_on" : "heat_off";
    const char* emcMessage = (emcState == 0) ? "emc_on" : "emc_off";
    const char* tempLimitMessage = (tempLimitState == 0) ? "templimit_on" : "templimit_off";
    const char* snowMessage = (snowState == 0) ? "snow_on" : "snow_off";
    const char* gfepMessage = (gfepState == 0) ? "gfep_on" : "gfep_off";

    char dataDEVICE[200];
    snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
    int data_len = strlen(dataDEVICE);
    char content_length[16];
    snprintf(content_length, sizeof(content_length), "%d", data_len);
    esp_http_client_handle_t client2 = esp_http_client_init(&config_put);
    printf("Making PUT Request in IO task initial...\n");
    // Set common headers for PUT requests
    esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4");
    esp_http_client_set_header(client2, "Content-Type", "application/json");
    esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
    esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
    printf("PUT request from state change... %s", dataDEVICE);
    esp_http_client_perform(client2);                                               // Peform PUT request
    vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
    esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks

    while (1)
    {
        // Read the input states
        int supplyState = gpio_get_level(Supply);
        int heatState = gpio_get_level(Heat);
        int emcState = gpio_get_level(EMC);
        int tempLimitState = gpio_get_level(Temp_Limit);
        int snowState = gpio_get_level(Snow);
        int gfepState = gpio_get_level(GFEP);

        // Check for changes and publish MQTT messages if needed
        if (supplyState != supplyLastState)
        {
            supplyLastState = supplyState;
            const char* supplyMessage = (supplyState == 0) ? "sup_on" : "sup_off";

            // Read the input states
            int heat_value = gpio_get_level(Heat);
            int emc_value = gpio_get_level(EMC);
            int temp_limit_value = gpio_get_level(Temp_Limit);
            int snow_value = gpio_get_level(Snow);
            int gfep_value = gpio_get_level(GFEP);
            int supply_value = (supplyState == 0) ? 0 : 1;
            printf("Making PUT Request...\n");

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
       
            publish_mqtt_message(mqttClient, serial_str, supplyMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        if (heatState != heatLastState)
        {
            heatLastState = heatState;
            const char* heatMessage = (heatState == 0) ? "heat_on" : "heat_off";
            
             // Read the input states
            int supply_value = gpio_get_level(Supply);
            int emc_value = gpio_get_level(EMC);
            int temp_limit_value = gpio_get_level(Temp_Limit);
            int snow_value = gpio_get_level(Snow);
            int gfep_value = gpio_get_level(GFEP);
            int heat_value = (heatState == 0) ? 0 : 1;

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
        
            publish_mqtt_message(mqttClient, serial_str, heatMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        if (emcState != emcLastState)
        {
            emcLastState = emcState;
            const char* emcMessage = (emcState == 0) ? "emc_on" : "emc_off";

            // Read the input states
            int heat_value = gpio_get_level(Heat);
            int supply_value = gpio_get_level(Supply);
            int temp_limit_value = gpio_get_level(Temp_Limit);
            int snow_value = gpio_get_level(Snow);
            int gfep_value = gpio_get_level(GFEP);
            int emc_value = (emcState == 0) ? 0 : 1;

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
        
            publish_mqtt_message(mqttClient, serial_str, emcMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        if (tempLimitState != tempLimitLastState)
        {
            tempLimitLastState = tempLimitState;
            const char* tempLimitMessage = (tempLimitState == 0) ? "templimit_on" : "templimit_off";

            // Read the input states
            int heat_value = gpio_get_level(Heat);
            int emc_value = gpio_get_level(EMC);
            int supply_value = gpio_get_level(Supply);
            int snow_value = gpio_get_level(Snow);
            int gfep_value = gpio_get_level(GFEP);
            int temp_limit_value = (tempLimitState == 0) ? 0 : 1;

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
        
            publish_mqtt_message(mqttClient, serial_str, tempLimitMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        if (snowState != snowLastState)
        {
            snowLastState = snowState;
            const char* snowMessage = (snowState == 0) ? "snow_on" : "snow_off";

            // Read the input states
            int heat_value = gpio_get_level(Heat);
            int emc_value = gpio_get_level(EMC);
            int temp_limit_value = gpio_get_level(Temp_Limit);
            int supply_value = gpio_get_level(Supply);
            int gfep_value = gpio_get_level(GFEP);
            int snow_value = (snowState == 0) ? 0 : 1;

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
       
            publish_mqtt_message(mqttClient, serial_str, snowMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        if (gfepState != gfepLastState)
        {
            gfepLastState = gfepState;
            const char* gfepMessage = (gfepState == 0) ? "gfep_on" : "gfep_off";

            // Read the input states
            int heat_value = gpio_get_level(Heat);
            int emc_value = gpio_get_level(EMC);
            int temp_limit_value = gpio_get_level(Temp_Limit);
            int snow_value = gpio_get_level(Snow);
            int supply_value = gpio_get_level(Supply);
            int gfep_value = (gfepState == 0) ? 0 : 1;

            client2 = esp_http_client_init(&config_put);       // Initalize PUT request

            // Send the PUT request
            char dataDEVICE[200];
            snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
            int data_len = strlen(dataDEVICE);
            char content_length[16];
            snprintf(content_length, sizeof(content_length), "%d", data_len);
            esp_http_client_set_header(client2, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); 
            esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
            esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
            esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
            printf("PUT request from state change... %s", dataDEVICE);
            esp_http_client_perform(client2);                                               // Peform PUT request
            vTaskDelay(300 / portTICK_PERIOD_MS);                                          // Wait 0.1 seconds
            esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks
        
            publish_mqtt_message(mqttClient, serial_str, gfepMessage);
            ESP_LOGI(TAG, "sent publish successful");
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    esp_http_client_cleanup(client2);
}

static void weather_task(void)
{
    weatherClient = esp_http_client_init(&config_get_weather);        // Initalize GET request

    vTaskDelay(300 / portTICK_PERIOD_MS);       // wait 100mS

    printf("Making weather GET request.... \n");
    esp_http_client_perform(weatherClient);            // makes GET request from database filtering by its serial number under devices

    printf("\nGET request done.... \n");
    vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
    esp_http_client_cleanup(weatherClient);       // clean GET request
    vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
    while (retryGetRequest == 0) {
        if (retryGetRequest == 1) {
            printf("Breaking\n");
            break;
        }
        for (num = 0; num <= 10; num++) {
            vTaskDelay(100 / portTICK_PERIOD_MS);                   // wait 100mS
            weatherClient = esp_http_client_init(&config_get_weather);        // Initalize GET request

            vTaskDelay(300 / portTICK_PERIOD_MS);       // wait 100mS
            esp_http_client_perform(weatherClient);   // makes GET request from database filtering by its serial number under devices
            printf("Made GET request again...\n");
            vTaskDelay(100 / portTICK_PERIOD_MS);                   // wait 100mS
            esp_http_client_cleanup(weatherClient);                   // clean GET request
            if (num == 10) {
                printf("Maximum retry count reached. Giving up.\n");
                // Reboot the ESP32
                printf("Restarting ESP32...\n");
                esp_restart();
            }
            printf("Num: %d\n", num);
            if (retryGetRequest == 1) {
                break;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);                   // wait 100mS
        }
    }
    while(1) {
       vTaskDelay(10000 / portTICK_PERIOD_MS);                   // wait 100mS 
       printf("Hello!\n");
    }
}


void app_main(void)
{
    // Setup IOs
    config_IOs();

    // Setup UART
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    // We won't use a buffer for sending data.
    uart_driver_install(UART, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART, &uart_config);
    uart_set_pin(UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Getting Base MAC & generate Serial Number
    esp_efuse_mac_get_default(baseMac);
    printf("MAC address BASE: %02X:%02X:%02X:%02X:%02X:%02X\n",                                                   
    baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
    for (int i = 0; i < 6; i++)
        {
            serial_number = (serial_number << 8) + baseMac[i];
        }

    // Convert serial_number to string using sprintf()
    sprintf(serial_str, "%" PRIu64, serial_number);
    printf("Serial_str: %s\n", serial_str);

    // Concatenate the url string and serial_str using strcat()
    strcat(url, serial_str);
    printf("URL: %s\n", url);

    size_t inputLength = strlen(serial_str);

    // Hash the input string
    hashString(serial_str, inputLength, hash);

    // Print the hash
    printf("Hash: ");
    printHash(hash, sizeof(hash));

    // Convert the hash to a string
    for (size_t i = 0; i < sizeof(hash); i++) {
        sprintf(&security_key[2 * i], "%02X", hash[i]);
    }
    security_key[2 * sizeof(hash)] = '\0';  // Add null terminator
    printf("Hash is now...: %s\n", security_key);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Read temperature data
    config_temp();

    // Test extern variable
    printf("-=-=-=-=-=-=-=-=-=-=Wifi Connected: %d\n", wifi_connected);
    // Start Wifi
    wifi_app_start();
    vTaskDelay(5000 / portTICK_PERIOD_MS); 
    startBLE();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler_wifi, NULL));

    while (wifi_connected == false) {
        printf("Not connected to WiFi, please configure...\n");
        vTaskDelay(3000 / portTICK_PERIOD_MS);      // Wait 10 seconds
      
    }

    if (wifi_connected == true) {
        printf("Successfully connected!\n");
        vTaskDelay(5000 / portTICK_PERIOD_MS);      // Wait 10 seconds
        
        // Make an initial requst to database and then use MQTT calls
        get_database_info();
        // Start MQTT connection
        mqtt_app_start();
        printf("Done with mqtt INITALIZATION\n");
        // More UART
        xTaskCreate(rx_task, "uart_rx_task", 1024*3, NULL, configMAX_PRIORITIES-1, NULL);
        xTaskCreate(tx_task, "uart_tx_task", 1024*3, NULL, configMAX_PRIORITIES-2, NULL);
        vTaskDelay(5000 / portTICK_PERIOD_MS); 
        xTaskCreate(ios_task, "read_ios_task", 1024*5, NULL, configMAX_PRIORITIES-1, NULL);
        xTaskCreate(weather_task, "get_weather_task", 1024*5, NULL, configMAX_PRIORITIES-1, NULL);
    }
}