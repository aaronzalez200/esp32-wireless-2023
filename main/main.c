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

#define DEFAULT_VREF    3300        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64          //Multisampling

// ADC setup
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_0;         // GPIO36
static const adc_bits_width_t width = ADC_WIDTH_BIT_10;     // 10-bit ADC
static const adc_atten_t atten = ADC_ATTEN_DB_11;           // Attenuation 
static const adc_unit_t unit = ADC_UNIT_1;

// Define variables to store the values of "Supply" and "Heat"
int supply_value = 0;                                           // supply value to send over HTTP
int heat_value = 0;                                             // heat value to send over HTTP
int emc_value = 0;                                              // emc heater on/off value to send over HTTP
int emc_toggle_value = 0;                                       // emc enable value to send over HTTP
int temp_limit_value = 0;                                       // temp limit value to send over HTTP
int snow_value = 0;                                             // snow value to send over HTTP
int gfep_value = 0;                                             // gfep value to send over HTTP
int temp_value = 0;                                           // temp value to send over HTTP
// Define variables to manage MAC Address to obtain Serial Number
uint8_t mac_buffer[6];                                          // mac address of ESP32; gets converted to a serial number
uint64_t serial_number = 0;                                     // used to convert MAC address to serial number for HTTP requests
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

TaskHandle_t myTaskHandle = NULL;

extern const uint8_t certificate_pem_start[] asm("_binary_certificate_pem_start");
extern const uint8_t certificate_pem_end[] 	 asm("_binary_certificate_pem_end");

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}

// Configure ADC for obtaining temperature value
void config_temp()
{
    // Configure ADC
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);

    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
}

// Read ADC to get temp using multisampling of 64
void read_and_print_adc_value()
{
    float adc_reading = 0;      // Hold ADC reading
    //Multisampling
    for (int i = 0; i < NO_OF_SAMPLES; i++) 
    {
        adc_reading += adc1_get_raw((adc1_channel_t)channel);
    }
    adc_reading /= NO_OF_SAMPLES;
    //Convert adc_reading to voltage in mV
    float voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    //printf("Raw: %f \tVoltage: %f mV\n", adc_reading, voltage);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // convert the value to resistance
    adc_reading = (1023 / adc_reading) - 1;
    adc_reading = 10000 / adc_reading;
    // convert to temperature
    float steinhart;
    steinhart = adc_reading / 10000;    // (R/Ro)
    steinhart = log(steinhart);         // ln(R/Ro)
    steinhart /= 3950;                  // 1/B * ln(R/Ro)
    steinhart += 1.0 / (25 + 273.15);   // + (1/To)
    steinhart = 1.0 / steinhart;        // Invert
    steinhart -= 273.15;                // convert absolute temp to C
    // printf("Temperature %.1f *C", steinhart); 
    steinhart = (steinhart*9/5) + 32;
    temp_value = steinhart;
    //printf("Temperature %.1f *F\n", steinhart);
}

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

// Handle HTTP GET requests
esp_err_t client_event_get_handler(esp_http_client_event_handle_t evt)  // Handles HTTP GET requests
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_DATA:
        {
            // printf("\n-----HTTP EVENT OCCURED-----\n");
            fwrite(evt->data, evt->data_len, 1, stdout);
            // Parse the received JSON data using cJSON
            cJSON *root = cJSON_Parse((char *)evt->data);
            if (root == NULL) {
                // printf("\nError parsing JSON: %s\n", cJSON_GetErrorPtr());
                break;
            }
            // printf("Moving on to devices array verification.....");
            // Extract the array of devices from the root object
            cJSON *devices_array = cJSON_GetObjectItemCaseSensitive(root, "devices");
            if (!cJSON_IsArray(devices_array)) {
                // printf("Error: devices is not an array\n");
                cJSON_Delete(root);
                break;
            }
            
            // Extract the first device object from the devices array
            cJSON *device_obj = cJSON_GetArrayItem(devices_array, 0);
            if (!cJSON_IsObject(device_obj)) {
                // printf("Error: device is not an object\n");
                cJSON_Delete(root);
                break;
            }
            
            // Extract the values of "Supply" and "Heat" from the device object
            cJSON *supply_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Supply");
            if (!cJSON_IsNumber(supply_value_obj)) {
                // printf("Error: Supply value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            supply_value = supply_value_obj->valueint;
            
            cJSON *heat_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Heat");
            if (!cJSON_IsNumber(heat_value_obj)) {
                // printf("Error: Heat value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            heat_value = heat_value_obj->valueint;

            cJSON *emc_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC");
            if (!cJSON_IsNumber(emc_value_obj)) {
                // printf("Error: EMC value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            emc_value = emc_value_obj->valueint;

            cJSON *snow_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Snow");
            if (!cJSON_IsNumber(snow_value_obj)) {
                // printf("Error: Snow value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            snow_value = snow_value_obj->valueint;

            cJSON *temp_limit_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "Temp_Limit");
            if (!cJSON_IsNumber(temp_limit_value_obj)) {
                // printf("Error: Temp Limit value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            temp_limit_value = temp_limit_value_obj->valueint;

            cJSON *gfep_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "GFEP");
            if (!cJSON_IsNumber(gfep_value_obj)) {
                // printf("Error: GFEP value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            gfep_value = gfep_value_obj->valueint;

            cJSON *emc_toggle_value_obj = cJSON_GetObjectItemCaseSensitive(device_obj, "EMC_Toggle");
            if (!cJSON_IsNumber(emc_toggle_value_obj)) {
                // printf("Error: EMC_Toggle value is not a number\n");
                cJSON_Delete(root);
                break;
            }
            emc_toggle_value = emc_toggle_value_obj->valueint;
            
           //  printf("Supply value: %d\n", supply_value);
            // printf("Heat value: %d\n", heat_value);
            // printf("EMC value: %d\n", emc_value);
            // printf("EMC_Toggle value: %d\n", emc_toggle_value);
            // printf("Snow value: %d\n", snow_value);
            // printf("GFEP value: %d\n", gfep_value);
            // printf("Temp Limit value: %d\n", temp_limit_value);
            // printf("=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
            // Cleanup cJSON memory
            cJSON_Delete(root);    
            break;
        }
        default:
            break;
    }
	return ESP_OK;
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

// Thread that gets database info every few seconds
static void get_database_info()         // Makes GET request from database filtering by its serial number under devices
{
	while(1) {
        //printf("----------------------------------");
        //printf("\nCheck URL: %s\n", url);
		esp_http_client_config_t config_get = {         // GET request config
			.url = url,
			.method = HTTP_METHOD_GET,
			.event_handler = client_event_get_handler,
			.auth_type = HTTP_AUTH_TYPE_BASIC,
			.transport_type = HTTP_TRANSPORT_OVER_TCP,
            .buffer_size = 1024
		};

        esp_http_client_config_t config_put = {        // PUT request config
            .url = url,
            .method = HTTP_METHOD_PUT,
            .event_handler = client_event_put_handler,
            .auth_type = HTTP_AUTH_TYPE_BASIC,
            .transport_type = HTTP_TRANSPORT_OVER_TCP
        };
        
		esp_http_client_handle_t client = esp_http_client_init(&config_get);        // Initalize GET request
        esp_http_client_set_header(client, "api_key", "vPgTEzvLl9lVZGOshej9ujmd7dr7qghGAftUAWeuuM8vsPBUya2Bw37996Djlbi4"); // Add API Key to header
        vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
        esp_http_client_handle_t client2 = esp_http_client_init(&config_put);       // Initalize PUT request
		esp_http_client_perform(client);            // make GET request
        vTaskDelay(100 / portTICK_PERIOD_MS);       // wait 100mS
		esp_http_client_cleanup(client);            // clean up 1st HTTP request; avoid memory leaks

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
        // printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
        // mac_buffer[0], mac_buffer[1], mac_buffer[2], mac_buffer[3], mac_buffer[4], mac_buffer[5];
        // printf("Making PUT Request...\n");

        // Send the PUT request
        char dataDEVICE[200];
        snprintf(dataDEVICE, sizeof(dataDEVICE), "{\"serial\":\"%s\",\"Supply\":%d,\"EMC\":%d,\"EMC_Toggle\":%d,\"Temp_Limit\":%d,\"Heat\":%d,\"Snow\":%d,\"GFEP\":%d, \"Temp_Value\":%d}", serial_str, supply_value, emc_value, emc_toggle_value, temp_limit_value, heat_value, snow_value, gfep_value, temp_value);
        int data_len = strlen(dataDEVICE);
        printf("DATA Device IS: %s", dataDEVICE);
        char content_length[16];
        snprintf(content_length, sizeof(content_length), "%d", data_len);
        esp_http_client_set_header(client2, "Content-Type", "application/json");        // Set header for PUT request
        esp_http_client_set_header(client2, "Content-Length", content_length);          // Set header for PUT request
        esp_http_client_set_post_field(client2, dataDEVICE, strlen(dataDEVICE));        // Set header for PUT request
        esp_http_client_perform(client2);                                               // Peform PUT request
        vTaskDelay(3000 / portTICK_PERIOD_MS);                                          // Wait 3 seconds
        esp_http_client_cleanup(client2);                                               // Clean up HTTP request; avoid memory leaks

        // Read ADC
        read_and_print_adc_value();
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

    // Read temperature data
    config_temp();

	// Start Wifi
	wifi_app_start();

	vTaskDelay(4000 / portTICK_PERIOD_MS);      // Wait 4 seconds
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

