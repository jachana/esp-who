#pragma once
#include <functional>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

namespace who {
namespace wakeup {

class WhoWakeupWord {
public:
    WhoWakeupWord();
    ~WhoWakeupWord();
    
    // Initialize the wake-up word detection
    bool init();
    
    // Start wake-up word detection task
    bool start();
    
    // Stop wake-up word detection
    void stop();
    
    // Set callback function to be called when wake-up word is detected
    void set_wakeup_callback(const std::function<void(const std::string&)>& callback);
    
    // Set the wake-up word to detect (default: "hello")
    void set_wakeup_word(const std::string& word);

private:
    static void audio_task(void* param);
    void process_audio();
    bool detect_wakeup_word(const int16_t* audio_data, size_t samples);
    bool simple_voice_activity_detection(const int16_t* audio_data, size_t samples);
    
    TaskHandle_t m_task_handle;
    std::function<void(const std::string&)> m_wakeup_callback;
    std::string m_wakeup_word;
    bool m_running;
    bool m_initialized;
    i2s_chan_handle_t m_rx_handle;
    
    // Audio configuration
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int BUFFER_SIZE = 1024;
    static constexpr int VOICE_THRESHOLD = 20;    // Further lowered threshold for microphone sensitivity
    static constexpr int MIN_VOICE_SAMPLES = 8000; // Minimum samples for voice detection
};

} // namespace wakeup
} // namespace who
