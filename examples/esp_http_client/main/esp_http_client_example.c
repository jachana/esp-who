/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "protocol_examples_utils.h"

// This is just for logging
static const char *TAG = "HTTP_CLIENT";

#define MAX_HTTP_OUTPUT_BUFFER 2048

//initialise a FIFO buffer that the mic can write into, and the https task can
//      read out of. This will prevent threading problems

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"    
#include "freertos/ringbuf.h"

// ESP32-S3-EYE internal microphone GPIO pins (matching who_wakeup_word configuration)
#define I2S_NUM         (1) // i2s port number (using I2S_NUM_1 like wakeup word)
#define I2S_SAMPLE_RATE (16000)
#define I2S_BCK_IO      (GPIO_NUM_41) // Bit Clock pin (BCLK)
#define I2S_WS_IO       (GPIO_NUM_42) // Word Select pin (WS/LRCLK)
#define I2S_DO_IO       (I2S_GPIO_UNUSED) // Data Out pin - not used for microphone
#define I2S_DI_IO       (GPIO_NUM_2) // Data In pin for microphone (DIN)
#define I2S_MCLK_IO     (I2S_GPIO_UNUSED) // Master clock not used
#define FIFO_SIZE       (64 * 1024) // Size of the FIFO buffer in bytes (increased to prevent overflow)
#define CHUNK_SIZE      (2048)      // Size of each audio chunk (matches I2S read size)
#define MIN_CHUNKS_TO_SEND (3)      // Minimum number of chunks to accumulate before sending
static uint8_t *fifo_buffer;
static SemaphoreHandle_t fifo_mutex;

static size_t fifo_bytes_available = 0;
static size_t fifo_write_pos = 0;
static size_t fifo_read_pos = 0;



// The PEM file was extracted from the output of this command:
//
//      openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null
//
// The CA root cert is the last cert given in the chain of certs.

extern const char connect_auras_root_cert_pem_start[] asm("_binary_connect_auras_root_cert_pem_start");
extern const char connect_auras_root_cert_pem_end[] asm("_binary_connect_auras_root_cert_pem_end");

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer; // Buffer to store response of http request from event handler
    static size_t output_len;   // Stores number of bytes read (FIXED: Changed from int to size_t)
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED - Connected to server");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT - Headers sent to server");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA - Received %d bytes from server", evt->data_len);
        if (evt->data_len > 0 && evt->data) {
            ESP_LOGI(TAG, "Server response: %.*s", evt->data_len, (char*)evt->data);
        }
        // Clean the buffer in case of a new request
        if (output_len == 0 && evt->user_data)
        {
            // we are just starting to copy the output data into the use
            memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
        }
        /*
         *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
         *  However, event handler can also be used in case chunked encoding is used.
         */
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            // If user_data buffer is configured, copy the response into the buffer
            size_t copy_len = 0; // FIXED: Changed from int to size_t for consistency
            if (evt->user_data)
            {
                // The last byte in evt->user_data is kept for the NULL character in case of out-of-bound access.
                copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                if (copy_len > 0) // FIXED: Explicit check for > 0
                {
                    memcpy(evt->user_data + output_len, evt->data, copy_len);
                }
            }
            else
            {
                int64_t content_len_raw = esp_http_client_get_content_length(evt->client);
                // FIXED: Proper validation of content length before casting
                if (content_len_raw <= 0 || content_len_raw > SIZE_MAX) {
                    ESP_LOGW(TAG, "Invalid content length: %" PRId64 ", skipping buffer allocation", content_len_raw);
                    return ESP_OK;
                }
                
                size_t content_len = (size_t)content_len_raw; // FIXED: Safe cast after validation
                
                if (output_buffer == NULL)
                {
                    // We initialize output_buffer with 0 because it is used by strlen() and similar functions therefore should be null terminated.
                    output_buffer = (char *)calloc(content_len + 1, sizeof(char));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                
                copy_len = MIN(evt->data_len, (content_len - output_len));
                if (copy_len > 0) // FIXED: Explicit check for > 0
                {
                    memcpy(output_buffer + output_len, evt->data, copy_len);
                }
            }
            output_len += copy_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
            // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED - Connection closed");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        esp_http_client_set_header(evt->client, "From", "user@example.com");
        esp_http_client_set_header(evt->client, "Accept", "text/html");
        esp_http_client_set_redirection(evt->client);
        break;
    }
    return ESP_OK;
}
static void mic_task(void *pvParameters)
{
    // Configure I2S for RX (microphone input)
    i2s_chan_handle_t rx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = I2S_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256, // Explicit MCLK multiple
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    
    // Log actual I2S configuration for debugging
    ESP_LOGI(TAG, "I2S Microphone Configuration:");
    ESP_LOGI(TAG, "  Sample Rate: %d Hz", I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "  Bit Width: 16-bit");
    ESP_LOGI(TAG, "  Channels: Mono");
    ESP_LOGI(TAG, "  MCLK Multiple: 256");

    while (1)
    {
        // Read samples from the mic and write them to the FIFO buffer
        size_t bytes_read = 0;
        int16_t samples[1024];  // 2048 bytes = 1024 int16_t samples
        esp_err_t ret = i2s_channel_read(rx_handle, (void *)samples, sizeof(samples), &bytes_read, portMAX_DELAY);
        
        if (ret == ESP_OK && bytes_read > 0 && bytes_read <= sizeof(samples) && (bytes_read % sizeof(int16_t)) == 0) {
            if (fifo_buffer == NULL || fifo_mutex == NULL) {
                ESP_LOGE(TAG, "FIFO buffer or mutex not initialized");
                break;
            }

            // Cast samples to bytes for processing
            const uint8_t *sample_bytes = (const uint8_t *)samples;

            xSemaphoreTake(fifo_mutex, portMAX_DELAY);
            if (fifo_bytes_available + bytes_read <= FIFO_SIZE)
            {
                // Copy int16_t samples as bytes to the FIFO buffer
                // This preserves the audio data integrity while storing as uint8_t stream
                for (size_t i = 0; i < bytes_read; i++) {
                    fifo_buffer[fifo_write_pos] = sample_bytes[i];
                    fifo_write_pos = (fifo_write_pos + 1) % FIFO_SIZE;
                }
                fifo_bytes_available += bytes_read;
            }
            else {
                ESP_LOGW(TAG, "FIFO buffer overflow, dropping samples (available: %zu, trying to add: %zu)",
                         fifo_bytes_available, bytes_read);
            }
            xSemaphoreGive(fifo_mutex);

            size_t samples_read = bytes_read / sizeof(int16_t);
            ESP_LOGI(TAG, "Read %zu bytes (%zu int16_t samples) from mic", bytes_read, samples_read);
            
            // Print raw bytes for debugging (first 32 bytes)
            size_t bytes_to_print = (bytes_read > 32) ? 32 : bytes_read;
            ESP_LOGI(TAG, "Raw bytes from mic (first %zu bytes):", bytes_to_print);
            for (size_t i = 0; i < bytes_to_print; i += 16) {
                size_t end = (i + 16 > bytes_to_print) ? bytes_to_print : i + 16;
                char hex_str[64] = {0};
                char *ptr = hex_str;
                for (size_t j = i; j < end; j++) {
                    ptr += sprintf(ptr, "%02x ", sample_bytes[j]);
                }
                ESP_LOGI(TAG, "  [%02zu-%02zu]: %s", i, end-1, hex_str);
            }
            
            // Print as int16_t values for comparison (first 8 samples)
            size_t samples_to_print = (samples_read > 8) ? 8 : samples_read;
            ESP_LOGI(TAG, "As int16_t values (first %zu samples):", samples_to_print);
            char sample_str[128] = {0};
            char *sptr = sample_str;
            for (size_t i = 0; i < samples_to_print; i++) {
                sptr += sprintf(sptr, "%d ", samples[i]);
            }
            ESP_LOGI(TAG, "  Samples: %s", sample_str);
        }
        else {
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Error reading from I2S: %d", ret);
            } else if (bytes_read == 0) {
                ESP_LOGW(TAG, "No data read from I2S");
            } else if ((bytes_read % sizeof(int16_t)) != 0) {
                ESP_LOGE(TAG, "Invalid bytes_read %zu, not aligned to int16_t", bytes_read);
            } else {
                ESP_LOGE(TAG, "Invalid bytes_read %zu, exceeds buffer size %zu", bytes_read, sizeof(samples));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Responsive delay
    }

    ESP_LOGI(TAG, "Cleaning up mic_task resources");
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    vTaskDelete(NULL);
}

static void tone_generator_task(void *pvParameters)
{
    // Generate test tone instead of reading from microphone
    ESP_LOGI(TAG, "Starting tone generator task for server testing");
    
    // Generate a 1kHz sine wave at 16kHz sample rate
    const float frequency = 1000.0f; // 1kHz tone
    const float sample_rate = 16000.0f;
    const float amplitude = 8000.0f; // Amplitude for 16-bit samples
    const size_t samples_per_chunk = 1024; // 1024 int16_t samples = 2048 bytes
    
    uint32_t sample_index = 0;
    
    while (1)
    {
        if (fifo_buffer == NULL || fifo_mutex == NULL) {
            ESP_LOGE(TAG, "FIFO buffer or mutex not initialized");
            break;
        }

        // Generate sine wave samples
        int16_t samples[samples_per_chunk];
        for (size_t i = 0; i < samples_per_chunk; i++) {
            float t = (float)(sample_index + i) / sample_rate;
            float sine_value = sinf(2.0f * M_PI * frequency * t);
            samples[i] = (int16_t)(sine_value * amplitude);
        }
        sample_index += samples_per_chunk;
        
        size_t bytes_to_write = samples_per_chunk * sizeof(int16_t); // 2048 bytes
        
        xSemaphoreTake(fifo_mutex, portMAX_DELAY);
        if (fifo_bytes_available + bytes_to_write <= FIFO_SIZE)
        {
            // Copy int16_t samples as bytes to the FIFO buffer
            const uint8_t *sample_bytes = (const uint8_t *)samples;
            for (size_t i = 0; i < bytes_to_write; i++) {
                fifo_buffer[fifo_write_pos] = sample_bytes[i];
                fifo_write_pos = (fifo_write_pos + 1) % FIFO_SIZE;
            }
            fifo_bytes_available += bytes_to_write;
        }
        else {
            ESP_LOGW(TAG, "FIFO buffer overflow, dropping samples (available: %zu, trying to add: %zu)",
                     fifo_bytes_available, bytes_to_write);
        }
        xSemaphoreGive(fifo_mutex);

        ESP_LOGI(TAG, "Generated %zu bytes (%zu int16_t samples) of 1kHz tone", bytes_to_write, samples_per_chunk);
        
        vTaskDelay(pdMS_TO_TICKS(50)); // Generate tone every 50ms
    }

    ESP_LOGI(TAG, "Tone generator task ended");
    vTaskDelete(NULL);
}

static void https_post_task(void *pvParameters)
{
    // Post to the server every .5 seconds
    while (1)
    {
        esp_err_t err;

        // esp_http_client_config_t config = {
        //     .url = "https://api.connect-auras.com/web/delete-demo-user",
        //     .event_handler = _http_event_handler,
        //     .cert_pem = connect_auras_root_cert_pem_start,
        //     .is_async = true,
        //     .timeout_ms = 5000,
        // };

        esp_http_client_config_t config = {
            .url = "http://192.168.1.142:8000/web/pcm-to-text",
            .event_handler = _http_event_handler,
            .cert_pem = connect_auras_root_cert_pem_start,
            .is_async = false,  // Changed to synchronous to avoid write issues
            .timeout_ms = 15000, // Increased timeout for larger payloads
            .buffer_size = 32768, // Increased buffer size to prevent allocation errors
            .buffer_size_tx = 16384, // Set explicit TX buffer size
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        // Check how many complete chunks are available
        size_t available_bytes = 0;
        xSemaphoreTake(fifo_mutex, portMAX_DELAY);
        available_bytes = fifo_bytes_available;
        xSemaphoreGive(fifo_mutex);
        
        size_t complete_chunks = available_bytes / CHUNK_SIZE;
        ESP_LOGI(TAG, "FIFO status: %zu bytes available, %zu complete chunks", available_bytes, complete_chunks);

        // Skip if we don't have enough complete chunks to send
        if (complete_chunks < MIN_CHUNKS_TO_SEND) {
            ESP_LOGI(TAG, "Not enough complete chunks (%zu < %d), waiting...", complete_chunks, MIN_CHUNKS_TO_SEND);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(200)); // Wait 0.2 seconds before checking again
            continue;
        }

        // Calculate how many chunks to send (limit to smaller size to avoid HTTP write errors)
        const size_t max_chunks_per_request = 4; // Max 4 chunks = 8KB per request (further reduced)
        size_t chunks_to_send = (complete_chunks > max_chunks_per_request) ? max_chunks_per_request : complete_chunks;
        size_t bytes_to_send = chunks_to_send * CHUNK_SIZE;

        // Allocate memory for post data - use uint8_t for binary audio data
        uint8_t *post_data = (uint8_t *)malloc(bytes_to_send);
        if (post_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for post data");
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Copy data from circular FIFO buffer byte by byte
        xSemaphoreTake(fifo_mutex, portMAX_DELAY);
        for (size_t i = 0; i < bytes_to_send; i++) {
            post_data[i] = fifo_buffer[fifo_read_pos];
            fifo_read_pos = (fifo_read_pos + 1) % FIFO_SIZE;
        }
        fifo_bytes_available -= bytes_to_send;
        xSemaphoreGive(fifo_mutex);

        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        esp_http_client_set_post_field(client, (const char *)post_data, bytes_to_send);
        
        ESP_LOGI(TAG, "Sending %zu chunks (%zu bytes) of audio data to server...", chunks_to_send, bytes_to_send);
        ESP_LOGI(TAG, "URL: http://192.168.1.142:8000/web/pcm-to-text");

        while (1)
        {
            err = esp_http_client_perform(client);
            if (err != ESP_ERR_HTTP_EAGAIN)
            {
                break;
            }
            ESP_LOGI(TAG, "Connection timed out... Retrying");
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            int64_t content_length = esp_http_client_get_content_length(client);
            ESP_LOGI(TAG, "HTTP Request SUCCESS - Status: %d, Content-Length: %" PRId64 ", Chunks sent: %zu (%zu bytes)",
                     status_code, content_length, chunks_to_send, bytes_to_send);
            
            // Log first few bytes of audio data for debugging
            ESP_LOGI(TAG, "Audio data sample (first 16 bytes): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     post_data[0], post_data[1], post_data[2], post_data[3],
                     post_data[4], post_data[5], post_data[6], post_data[7],
                     post_data[8], post_data[9], post_data[10], post_data[11],
                     post_data[12], post_data[13], post_data[14], post_data[15]);
        }
        else
        {
            ESP_LOGE(TAG, "HTTP Request FAILED - Error: %s (0x%x)", esp_err_to_name(err), err);
            
            // If it's a write data error, try with smaller chunks next time
            if (err == ESP_ERR_HTTP_WRITE_DATA) {
                ESP_LOGW(TAG, "Write data error - payload might be too large (%zu bytes)", bytes_to_send);
            }
        }

        esp_http_client_cleanup(client);
        free(post_data);
        
        // Wait before next request (reduced delay to process audio faster)
        vTaskDelay(pdMS_TO_TICKS(100)); // 0.1 second delay to reduce buffer overflow
    }
    // TODO Close the while loop here and remove this vTaskDelete as you want the loop to go on forever
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Initialise chip
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Initialize FIFO buffer and mutex
    fifo_buffer = (uint8_t*)malloc(FIFO_SIZE);
    if (fifo_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate FIFO buffer");
        return;
    }
    memset(fifo_buffer, 0, FIFO_SIZE);
    fifo_mutex = xSemaphoreCreateMutex();
    if (fifo_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create FIFO mutex");
        free(fifo_buffer);
        return;
    }

    // Initialise network
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP");

    // Start the http post task
    xTaskCreate(&https_post_task, "https_post_task", 8192, NULL, 5, NULL);

    // Start the tone generator task for server testing (commented out - using mic now)
    // xTaskCreate(&tone_generator_task, "tone_generator_task", 8192, NULL, 5, NULL);

    // Start the mic task - now active for real microphone data
    xTaskCreate(&mic_task, "mic_task", 8192, NULL, 5, NULL);
}
