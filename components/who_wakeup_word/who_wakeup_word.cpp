#include "who_wakeup_word.hpp"
#include <cstring>
#include <cmath>

static const char* TAG = "WhoWakeupWord";

namespace who {
namespace wakeup {

WhoWakeupWord::WhoWakeupWord() 
    : m_task_handle(nullptr)
    , m_wakeup_word("hello")
    , m_running(false)
    , m_initialized(false)
    , m_rx_handle(nullptr)
{
}

WhoWakeupWord::~WhoWakeupWord()
{
    stop();
}

bool WhoWakeupWord::init()
{
    if (m_initialized) {
        return true;
    }

    // Create I2S channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &m_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return false;
    }

    // Configure I2S standard configuration
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_41,
            .ws = GPIO_NUM_42,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_2,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(m_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(m_rx_handle);
        m_rx_handle = nullptr;
        return false;
    }

    ret = i2s_channel_enable(m_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(m_rx_handle);
        m_rx_handle = nullptr;
        return false;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "Wake-up word detection initialized");
    return true;
}

bool WhoWakeupWord::start()
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "Wake-up word detection not initialized");
        return false;
    }

    if (m_running) {
        ESP_LOGW(TAG, "Wake-up word detection already running");
        return true;
    }

    m_running = true;
    
    BaseType_t ret = xTaskCreate(
        audio_task,
        "wakeup_word_task",
        4096,
        this,
        5,
        &m_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake-up word task");
        m_running = false;
        return false;
    }

    ESP_LOGI(TAG, "Wake-up word detection started");
    return true;
}

void WhoWakeupWord::stop()
{
    if (!m_running) {
        return;
    }

    m_running = false;
    
    if (m_task_handle) {
        vTaskDelete(m_task_handle);
        m_task_handle = nullptr;
    }

    if (m_initialized && m_rx_handle) {
        i2s_channel_disable(m_rx_handle);
        i2s_del_channel(m_rx_handle);
        m_rx_handle = nullptr;
        m_initialized = false;
    }

    ESP_LOGI(TAG, "Wake-up word detection stopped");
}

void WhoWakeupWord::set_wakeup_callback(const std::function<void(const std::string&)>& callback)
{
    m_wakeup_callback = callback;
    ESP_LOGI(TAG, "Wakeup callback set: %s", m_wakeup_callback ? "YES" : "NO");
}

void WhoWakeupWord::set_wakeup_word(const std::string& word)
{
    m_wakeup_word = word;
}

void WhoWakeupWord::audio_task(void* param)
{
    WhoWakeupWord* instance = static_cast<WhoWakeupWord*>(param);
    instance->process_audio();
}

void WhoWakeupWord::process_audio()
{
    int16_t* audio_buffer = new int16_t[BUFFER_SIZE];
    size_t bytes_read = 0;
    
    ESP_LOGI(TAG, "Audio processing task started - m_running: %d", m_running);
    
    while (m_running) {
        // Read audio data from I2S
        esp_err_t ret = i2s_channel_read(m_rx_handle, audio_buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read I2S data: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t samples_read = bytes_read / sizeof(int16_t);
        
        if (samples_read > 0) {
            // Calculate RMS energy for debugging
            long long sum_squares = 0;
            for (size_t i = 0; i < samples_read; i++) {
                sum_squares += (long long)audio_buffer[i] * audio_buffer[i];
            }
            double rms = sqrt((double)sum_squares / samples_read);
            
            // Voice activity detection and wake-up word detection
            if (simple_voice_activity_detection(audio_buffer, samples_read)) {
                if (detect_wakeup_word(audio_buffer, samples_read)) {
                    ESP_LOGI(TAG, "Wake-up word '%s' detected! (Energy: %.0f)", m_wakeup_word.c_str(), rms);
                    
                    if (m_wakeup_callback) {
                        m_wakeup_callback("Hello!");
                    }
                    
                    // Add a small delay to avoid multiple detections
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            } else {
                // Display current energy when no voice activity
                static int display_counter = 0;
                if (++display_counter >= 10) { // Update display every ~100ms
                    display_counter = 0;
                    if (m_wakeup_callback) {
                        char energy_str[32];
                        snprintf(energy_str, sizeof(energy_str), "Listening... %.0f", rms);
                        m_wakeup_callback(std::string(energy_str));
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    delete[] audio_buffer;
    ESP_LOGI(TAG, "Audio processing task ended");
}

bool WhoWakeupWord::simple_voice_activity_detection(const int16_t* audio_data, size_t samples)
{
    // Calculate RMS (Root Mean Square) for voice activity detection
    long long sum_squares = 0;
    for (size_t i = 0; i < samples; i++) {
        sum_squares += (long long)audio_data[i] * audio_data[i];
    }
    
    double rms = sqrt((double)sum_squares / samples);
    
    // Return true if RMS exceeds threshold (indicating voice activity)
    return rms > VOICE_THRESHOLD;
}

bool WhoWakeupWord::detect_wakeup_word(const int16_t* audio_data, size_t samples)
{
    // Simplified wake-up word detection - triggers on any sustained voice activity
    static int voice_activity_counter = 0;
    static int silence_counter = 0;
    
    if (simple_voice_activity_detection(audio_data, samples)) {
        voice_activity_counter++;
        silence_counter = 0;
        
        // Trigger after just 3 consecutive voice activity detections (~30ms of speech)
        if (voice_activity_counter >= 3) {
            voice_activity_counter = 0;
            ESP_LOGI(TAG, "Voice activity sustained - triggering wake-up word");
            return true;
        }
    } else {
        silence_counter++;
        // Reset voice counter after 2 silence detections
        if (silence_counter >= 2) {
            voice_activity_counter = 0;
        }
    }
    
    return false;
}

} // namespace wakeup
} // namespace who
