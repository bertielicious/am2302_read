#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define DATA 19

enum { LO = 0, HI = 1 };

static const char *TAG = "am2302";

static gptimer_handle_t bertie;// gptimer handle with global scope, so can be seen by ISR

volatile uint64_t high_pulse_length = 0;
volatile uint8_t i = 0;// index for array capturing sensor data
volatile bool arr[40];//array to hold 40 bits of data | 16 bits humidity | 16 bits temperature | 8 bits checksum

// ISR: capture HIGH pulse length → determine bit
static void IRAM_ATTR am2302_isr(void *arg)
{
    bool level = gpio_get_level(DATA);// read the logic level of DATA gpio 19
    uint64_t ts = 0;// create a timestamp variable

    gptimer_get_raw_count(bertie, &ts);// read elapsed time since last edge

    if (level == LO) {
        // Falling edge: HIGH pulse just ended
        high_pulse_length = ts;

        if (i < 40) {
            //arr[i] = (high_pulse_length > 40) ? HI : LO;
            if (high_pulse_length > 40) 
            {
                arr[i] = HI;
            } 
            else 
            {
                arr[i] = LO;
            }
            i++;
        }
    }

    // Reset timer for next pulse
    gptimer_set_raw_count(bertie, 0);//After each measurement, we reset the timer to zero so the next edge measures the next segment
}

void app_main(void)
{
     //initialise gpio 19 to send start signal to sensor, initially set as an output pin
    gpio_config_t phil = {
        .pin_bit_mask = 1ULL << DATA,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,   // external pull-up 4.7k to 10k ohms
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&phil);

    // Install ISR service (but DO NOT attach handler yet)
    gpio_install_isr_service(0);

    // configure gptimer as a free running timer with timestamp
    
    //configure timer parameters
    gptimer_config_t tonya = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000//each tick = 1 / resolution_hz seconds = 1us
    };

    gptimer_new_timer(&tonya, &bertie);//create new timer
    gptimer_enable(bertie);//before starting the timer, it must be enabled
    gptimer_start(bertie);//start the timer counting up from 0

    while (1)
    {
        //clear array ready to receive sensor data
        i = 0;
        for (int k = 0; k < 40; k++) 
        {
            arr[k] = 0;
        }

        // Ensure ISR is not active during start signal
        gpio_isr_handler_remove(DATA);

        //send start signal to sensor, gpio 19 still configured as output pin
        gpio_set_direction(DATA, GPIO_MODE_OUTPUT_OD);
        gpio_set_level(DATA, LO);
        vTaskDelay(pdMS_TO_TICKS(10));// pull line low for 10ms

        gpio_set_level(DATA, HI);
        esp_rom_delay_us(40);// pull line high for 40us

        gpio_set_direction(DATA, GPIO_MODE_INPUT);// gpio 19 now configured as an input pin, to receive sensor asknowledgement and humidity/ temperature data

        // === SENSOR RESPONSE ===
        while (gpio_get_level(DATA) == HI); // wait for LOW, 80us low pulse from sensor
        while (gpio_get_level(DATA) == LO); // wait for HIGH, 80us high pulse from sensor
        while (gpio_get_level(DATA) == HI); // wait for LOW (start of data)

        // 🔑 Reset timer exactly before data stream
        gptimer_set_raw_count(bertie, 0);

        // 🔑 Enable ISR for data capture
        gpio_isr_handler_add(DATA, am2302_isr, NULL);

        // Wait for full transmission (~4 ms)
        esp_rom_delay_us(5000);

        // Disable ISR after capture
        gpio_isr_handler_remove(DATA);

        if (i >= 40)
        {
            uint8_t data[5] = {0};

        // Convert bits → bytes
        int byte_index = 0;
        int bit_index = 0;

        for (int j = 0; j < 40; j++) 
        {
            // Shift current byte left to make space for next bit
            data[byte_index] = data[byte_index] << 1;

        // Add the new bit (0 or 1)
        if (arr[j] == 1) 
        {
            data[byte_index] = data[byte_index] | 1;
        }

        bit_index++;

    // After 8 bits, move to next byte
    if (bit_index == 8) {
        bit_index = 0;
        byte_index++;
    }
}

            uint16_t humidity_raw = (data[0] << 8) | data[1];
            uint16_t temp_raw = (data[2] << 8) | data[3];
            uint8_t checksum = data[4];

            uint8_t calc_checksum = data[0] + data[1] + data[2] + data[3];

            if (calc_checksum == checksum)
            {
                float humidity = humidity_raw / 10.0;
                float temperature = temp_raw / 10.0;

                // Handle negative temperatures
                if (temp_raw & 0x8000) {
                    temperature = -(temp_raw & 0x7FFF) / 10.0;
                }

                ESP_LOGI(TAG, "Humidity: %.1f %%", humidity);
                ESP_LOGI(TAG, "Temperature: %.1f C", temperature);
            }
            else
            {
                ESP_LOGE(TAG, "Checksum failed! Got 0x%02X expected 0x%02X",
                         checksum, calc_checksum);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Incomplete data received (%d bits)", i);
        }

        // Sensor requires ~2 seconds between reads
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
/*I (20381) am2302: Humidity: 60.4 %
I (20381) am2302: Temperature: 19.8 C
I (22391) am2302: Humidity: 60.2 %
I (22391) am2302: Temperature: 19.8 C*/