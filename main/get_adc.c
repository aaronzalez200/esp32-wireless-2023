#include "get_adc.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "math.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEFAULT_VREF    3300        // Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64          // Multisampling

static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_0;         // GPIO36
static const adc_bits_width_t width = ADC_WIDTH_BIT_10;     // 10-bit ADC
static const adc_atten_t atten = ADC_ATTEN_DB_11;           // Attenuation 
static const adc_unit_t unit = ADC_UNIT_1;

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

void config_temp()
{
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
    printf("CONFIGED!");
}

void read_and_print_adc_value(int* temp_value)
{
    float adc_reading = 0;
    for (int i = 0; i < NO_OF_SAMPLES; i++) 
    {
        adc_reading += adc1_get_raw((adc1_channel_t)channel);
    }
    adc_reading /= NO_OF_SAMPLES;
    float voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    vTaskDelay(pdMS_TO_TICKS(1000));
    adc_reading = (1023 / adc_reading) - 1;
    adc_reading = 10000 / adc_reading;
    float steinhart;
    steinhart = adc_reading / 10000;
    steinhart = log(steinhart);
    steinhart /= 3950;
    steinhart += 1.0 / (25 + 273.15);
    steinhart = 1.0 / steinhart;
    steinhart -= 273.15;
    steinhart = (steinhart*9/5) + 32;
    printf("steinhart: %f", steinhart);
    *temp_value = steinhart;
}