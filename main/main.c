/**
 * Application entry point.
 */
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

// Define variables to store the values of "Supply" and "Heat"
int supply_value = 0;
int heat_value = 0;
int emc_value = 0;
int emc_toggle_value = 0;
int temp_limit_value = 0;
int snow_value = 0;
int gfep_value = 0;
uint8_t mac_buffer[6];
uint64_t serial_number = 0;
gpio_num_t Supply = GPIO_NUM_18;
gpio_num_t Heat = GPIO_NUM_5;
gpio_num_t EMC = GPIO_NUM_19;
gpio_num_t GFEP = GPIO_NUM_21; 
gpio_num_t Temp_Limit = GPIO_NUM_22;
gpio_num_t Snow = GPIO_NUM_23;
gpio_num_t EMC_ON = GPIO_NUM_32;
gpio_num_t EMC_OFF = GPIO_NUM_33;

// Declare the url string and a buffer to hold the concatenated string
char url[100] = "https://heartfelt-pony-29b78c.netlify.app/api/devices?serial=";
char serial_str[20];

TaskHandle_t myTaskHandle = NULL;

extern const uint8_t certificate_pem_start[] asm("_binary_certificate_pem_start");
extern const uint8_t certificate_pem_end[] 	 asm("_binary_certificate_pem_end");

// Set GPIO pins 36 and 39 as output
void config_IOs() 
{
    gpio_set_direction(EMC_ON, GPIO_MODE_OUTPUT);
    gpio_set_direction(EMC_OFF, GPIO_MODE_OUTPUT);
    gpio_set_direction(EMC, GPIO_MODE_OUTPUT);
    gpio_set_direction(Supply, GPIO_MODE_INPUT);
    gpio_pullup_en(Supply);
    gpio_set_direction(Heat, GPIO_MODE_INPUT);
    gpio_pullup_en(Heat);
    gpio_set_direction(GFEP, GPIO_MODE_INPUT);
    gpio_pullup_en(GFEP);
    gpio_set_direction(Temp_Limit, GPIO_MODE_INPUT);
    gpio_pullup_en(Temp_Limit);
    gpio_set_direction(Snow, GPIO_MODE_INPUT);
    gpio_pullup_en(Snow);
}

esp_err_t client_event_get_handler(esp_http_client_event_handle_t evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
        {
            printf("\n-----HTTP EVENT OCCURED-----\n");
            fwrite(evt->data, evt->data_len, 1, stdout);
            // Parse the received JSON data using cJSON
            cJSON *root = cJSON_Parse((char *)evt->data);
            if (root == NULL) {
                printf("\nError parsing JSON: %s\n", cJSON_GetErrorPtr());
                break;
            }
            printf("Moving on to devices array verification.....");
            // Extract the array of devices from the root object
            cJSON *devices_array = cJSON_GetObjectItemCaseSensitive(root, "devices");
            if (!cJSON_IsArray(devices_array)) {
                printf("Error: devices is not an array\n");
                cJSON_Delete(root);
                break;
            }
            
            // Extract the first device object from the devices array
            cJSON *device_obj = cJSON_GetArrayItem(devices_array, 0);
            if (!cJSON_IsObject(device_obj)) {
                printf("Error: device is not an object\n");
                cJSON_Delete(root);
                break;
            }
            
            // Extract the values of "Supply" and "Heat" from the device object
            cJSON *supply_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Supply");
            if (!cJSON_IsNumber(supply_value_obj)) {
                printf("Error: Supply value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            supply_value = supply_value_obj->valueint;
            
            cJSON *heat_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Heat");
            if (!cJSON_IsNumber(heat_value_obj)) {
                printf("Error: Heat value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            heat_value = heat_value_obj->valueint;

            cJSON *emc_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC");
            if (!cJSON_IsNumber(emc_value_obj)) {
                printf("Error: EMC value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            emc_value = emc_value_obj->valueint;

            cJSON *snow_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Snow");
            if (!cJSON_IsNumber(snow_value_obj)) {
                printf("Error: Snow value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            snow_value = snow_value_obj->valueint;

            cJSON *temp_limit_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Temp_Limit");
            if (!cJSON_IsNumber(temp_limit_value_obj)) {
                printf("Error: Temp Limit value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            temp_limit_value = temp_limit_value_obj->valueint;

            cJSON *gfep_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "GFEP");
            if (!cJSON_IsNumber(gfep_value_obj)) {
                printf("Error: GFEP value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            gfep_value = gfep_value_obj->valueint;

            cJSON *emc_toggle_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC_Toggle");
            if (!cJSON_IsNumber(emc_toggle_value_obj)) {
                printf("Error: EMC_Toggle value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            emc_toggle_value = emc_toggle_value_obj->valueint;
            
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
        default:
            break;
    }
	return ESP_OK;
}

esp_err_t client_event_put_handler(esp_http_client_event_t *evt)
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

static void get_database_info()
{
	while(1) {
        printf("----------------------------------");
        printf("\nCheck URL: %s\n", url);
		esp_http_client_config_t config_get = {
			.url = url,
			.method = HTTP_METHOD_GET,
			.event_handler = client_event_get_handler,
			.auth_type = HTTP_AUTH_TYPE_NONE,
			.transport_type = HTTP_TRANSPORT_OVER_TCP,
            .buffer_size = 1024
		};

        esp_http_client_config_t config_put = {
            .url = url,
            .method = HTTP_METHOD_PUT,
            .event_handler = client_event_put_handler,
            .auth_type = HTTP_AUTH_TYPE_NONE,
            .transport_type = HTTP_TRANSPORT_OVER_TCP
        };
		esp_http_client_handle_t client = esp_http_client_init(&config_get);
        esp_http_client_handle_t client2 = esp_http_client_init(&config_put);
		esp_http_client_perform(client);
		esp_http_client_cleanup(client);
        if (gpio_get_level(Heat) == 1) {
            printf("Heat is low\n");
            heat_value = 1;
        } else {
            printf("Heat is high\n");
            heat_value = 0;
        }
        if (gpio_get_level(Supply) == 1) {
            printf("Supply is low\n");
            supply_value = 1;
        } else {
            printf("Supply is high\n");
            supply_value = 0;
        }
        if (emc_value == 1) {
            printf("EMC is low\n");
            gpio_set_level(EMC, 0);
            // Turn off override
            gpio_set_level(GPIO_NUM_32, 1); //EMC ON so set override ON HIGH
            gpio_set_level(GPIO_NUM_33, 1); //EMC ON so set override OFF HIGH
        } else {
            printf("EMC is high\n");
            gpio_set_level(EMC, 1);
            // If Override ON is selected & EMC is enabled
            if (emc_toggle_value == 1) {
                printf("EMC enabled, EMC Override ON\n");
                gpio_set_level(EMC_ON, 0); //EMC ON so set override ON LOW
                gpio_set_level(EMC_OFF, 1); //EMC ON so set override OFF HIGH
                // If Override OFF is selected & EMC is enabled
            } else {
                printf("EMC enabled, EMC Override OFF\n");
                gpio_set_level(EMC_ON, 1); //EMC ON so set override ON LOW
                gpio_set_level(EMC_OFF, 0); //EMC ON so set override OFF HIGH
            }
        } 
        if (gpio_get_level(Snow) == 1) {
            printf("Snow is low \n");
            snow_value = 1;
        } else {
            printf("Snow is high\n");
            snow_value = 0;
        }
        if (gpio_get_level(GFEP) == 1) {
            printf("GFEP is low\n");
            gfep_value = 1;
        } else {
            printf("GFEP is high\n");
            gfep_value = 0;
        }
        if (gpio_get_level(Temp_Limit) == 1) {
            printf("Temp Limit is low\n");
            temp_limit_value = 1;
        } else {
            printf("Temp Limit is high\n");
            temp_limit_value = 0;
        }
        printf("serial: %s, Supply: %i, EMC: %i, EMC_Toggle: %i, Temp_Limit: %i, Heat: %i, Snow: %i, GFEP: %i", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value);
        // Here is where we get the unique MAC ID of the ESP32 Chip 
        esp_wifi_get_mac(WIFI_IF_STA, mac_buffer);
        printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac_buffer[0], mac_buffer[1], mac_buffer[2], mac_buffer[3], mac_buffer[4], mac_buffer[5]);
        // Send the PUT request
        printf("Making PUT Request...\n");
        char dataDEVICE[200];
        snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value);
        int data_len = strlen(dataDEVICE);
        printf("DATA Device IS: %s", dataDEVICE);
        char content_length[16];
        snprintf(content_length, sizeof(content_length), "%d", data_len);
        esp_http_client_set_header(client2, "Content-Type", "application/json");
        esp_http_client_set_header(client2, "Content-Length", content_length);
        esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));
        esp_http_client_perform(client2);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_http_client_cleanup(client2);
    }
}

void app_main(void)
{
    // Setup IOs
    config_IOs();

    // Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// Start Wifi
	wifi_app_start();

	vTaskDelay(4000 / portTICK_PERIOD_MS);
	printf("We are connected to WIFI, now lets make GET requests");

    // Get MAC Address & convert into serial number
    esp_wifi_get_mac(WIFI_IF_STA, mac_buffer);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac_buffer[0], mac_buffer[1], mac_buffer[2], mac_buffer[3], mac_buffer[4], mac_buffer[5]);
    for (int i = 0; i < 6; i++)
    {
        serial_number = (serial_number << 8) + mac_buffer[i];
    }

    // Convert serial_number to string using sprintf()
    sprintf(serial_str, "%" PRIu64, serial_number);
    printf("Serial_str: %s", serial_str);

    // Concatenate the url string and serial_str using strcat()
    strcat(url, serial_str);
    printf("URL: %s", url);

	//get_database_info();
	xTaskCreate(get_database_info, "get_database_info", 4096, NULL, 10, &myTaskHandle);
}

