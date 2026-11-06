#if defined(AMPLIFIER_TEST)
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

namespace {
constexpr i2s_port_t kAmpPort = I2S_NUM_1;
constexpr int kAmpBclk = 13;
constexpr int kAmpLrc = 12;
constexpr int kAmpDin = 11;
constexpr uint32_t kSampleRate = 24000;
constexpr size_t kFramesPerChunk = 144;
constexpr float kTau = 6.28318530718f;
uint8_t volumePercent = 100;

struct ToneSegment {
    uint16_t durationMs;
    float startHz;
    float endHz;
    float level;
};

constexpr ToneSegment kCute[] = {
    {110, 720, 980, 0.30f}, {45, 0, 0, 0},
    {145, 1050, 1420, 0.34f},
};
constexpr ToneSegment kYes[] = {
    {105, 650, 900, 0.30f}, {125, 980, 1240, 0.32f},
};
constexpr ToneSegment kNo[] = {
    {155, 690, 500, 0.30f}, {145, 570, 390, 0.27f},
};
constexpr ToneSegment kScared[] = {
    {75, 850, 1190, 0.31f}, {70, 1180, 1580, 0.34f},
    {95, 1480, 1030, 0.27f},
};
constexpr ToneSegment kShy[] = {
    {100, 730, 860, 0.20f}, {60, 0, 0, 0},
    {125, 820, 680, 0.18f},
};
constexpr ToneSegment kWakeup[] = {
    {115, 460, 710, 0.26f}, {130, 750, 1030, 0.30f},
    {105, 1080, 1250, 0.25f},
};
constexpr ToneSegment kSleepy[] = {
    {220, 500, 405, 0.16f}, {95, 0, 0, 0},
    {260, 420, 325, 0.17f},
};
constexpr ToneSegment kAngry[] = {
    {130, 430, 325, 0.34f}, {55, 0, 0, 0},
    {180, 385, 250, 0.36f},
};

bool startI2s() {
    const i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = kSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    const i2s_pin_config_t pins = {
        .bck_io_num = kAmpBclk,
        .ws_io_num = kAmpLrc,
        .data_out_num = kAmpDin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    esp_err_t result = i2s_driver_install(kAmpPort, &config, 0, nullptr);
    if (result != ESP_OK) {
        Serial.printf("[audio] I2S driver install failed: %s\n", esp_err_to_name(result));
        return false;
    }
    result = i2s_set_pin(kAmpPort, &pins);
    if (result != ESP_OK) {
        Serial.printf("[audio] I2S pin setup failed: %s\n", esp_err_to_name(result));
        i2s_driver_uninstall(kAmpPort);
        return false;
    }
    return true;
}

bool writeSegment(uint32_t durationMs, float startHz, float endHz, float level) {
    int16_t frames[kFramesPerChunk * 2];
    const uint32_t totalFrames = durationMs * kSampleRate / 1000;
    float phase = 0.0f;
    for (uint32_t base = 0; base < totalFrames; base += kFramesPerChunk) {
        const uint32_t count = min((uint32_t)kFramesPerChunk, totalFrames - base);
        for (uint32_t i = 0; i < count; ++i) {
            const float progress = (float)(base + i) / (float)totalFrames;
            float sampleValue = 0.0f;
            if (startHz > 0.0f || endHz > 0.0f) {
                const float frequency = startHz + (endHz - startHz) * progress;
                phase += kTau * frequency / kSampleRate;
                if (phase >= kTau) phase -= kTau;
                const float envelope = min(1.0f, min(progress * 18.0f,
                                                        (1.0f - progress) * 18.0f));
                sampleValue = sinf(phase) * envelope * level *
                              (volumePercent / 100.0f) * 32760.0f;
            }
            const int16_t sample = (int16_t)sampleValue;
            frames[i * 2] = sample;
            frames[i * 2 + 1] = sample;
        }
        size_t bytesWritten = 0;
        const size_t bytesToWrite = count * sizeof(int16_t) * 2;
        const esp_err_t result = i2s_write(kAmpPort, frames, bytesToWrite,
                                           &bytesWritten, portMAX_DELAY);
        if (result != ESP_OK || bytesWritten != bytesToWrite) {
            Serial.printf("[audio] I2S write failed: %s (%u/%u bytes)\n",
                          esp_err_to_name(result), (unsigned)bytesWritten,
                          (unsigned)bytesToWrite);
            return false;
        }
    }
    return true;
}

void playTone(uint32_t durationMs) {
    Serial.printf("[audio] playing 440 Hz tone for %lums\n",
                  (unsigned long)durationMs);
    if (!writeSegment(durationMs, 440.0f, 440.0f, 0.22f)) return;
    i2s_zero_dma_buffer(kAmpPort);
    Serial.println("[audio] tone finished");
}

void playPattern(const char* name, const ToneSegment* pattern, size_t count) {
    uint32_t durationMs = 20;
    for (size_t i = 0; i < count; ++i) durationMs += pattern[i].durationMs;
    Serial.printf("[audio] playing %s %lums\n", name, (unsigned long)durationMs);
    for (size_t i = 0; i < count; ++i) {
        if (!writeSegment(pattern[i].durationMs, pattern[i].startHz,
                          pattern[i].endHz, pattern[i].level)) return;
    }
    delay(20);
    i2s_zero_dma_buffer(kAmpPort);
    Serial.printf("[audio] finished %s\n", name);
}

void setPins(int bclk, int lrc, int din) {
    const i2s_pin_config_t pins = {
        .bck_io_num = bclk,
        .ws_io_num = lrc,
        .data_out_num = din,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    const esp_err_t result = i2s_set_pin(kAmpPort, &pins);
    if (result != ESP_OK) {
        Serial.printf("[audio] I2S pin setup failed: %s\n", esp_err_to_name(result));
    }
}

void runPinTest() {
    const int pins[] = {kAmpBclk, kAmpLrc, kAmpDin};
    constexpr uint8_t orders[][3] = {
        {0, 1, 2}, {1, 0, 2}, {0, 2, 1},
        {2, 1, 0}, {1, 2, 0}, {2, 0, 1},
    };
    for (uint8_t test = 0; test < 6; ++test) {
        const int bclk = pins[orders[test][0]];
        const int lrc = pins[orders[test][1]];
        const int din = pins[orders[test][2]];
        Serial.printf("[audio] pintest %u: %u beep(s) BCLK=%d LRC=%d DIN=%d\n",
                      test + 1, test + 1, bclk, lrc, din);
        setPins(bclk, lrc, din);
        for (uint8_t beep = 0; beep <= test; ++beep) {
            if (!writeSegment(160, 880.0f, 880.0f, 0.45f) ||
                !writeSegment(180, 0.0f, 0.0f, 0.0f)) {
                setPins(kAmpBclk, kAmpLrc, kAmpDin);
                return;
            }
        }
        if (!writeSegment(700, 0.0f, 0.0f, 0.0f)) break;
    }
    i2s_zero_dma_buffer(kAmpPort);
    setPins(kAmpBclk, kAmpLrc, kAmpDin);
    Serial.printf("[audio] pintest done; restored BCLK=%d LRC=%d DIN=%d\n",
                  kAmpBclk, kAmpLrc, kAmpDin);
}

bool setVolume(const String& value) {
    if (value.isEmpty()) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (!isDigit(value[i])) return false;
    }
    const int requested = value.toInt();
    if (requested > 100) {
        Serial.println("[audio] volume must be from 0 to 100.");
        return true;
    }
    volumePercent = (uint8_t)requested;
    Serial.printf("[audio] volume=%u%%\n", volumePercent);
    return true;
}

void handleCommand(String command) {
    command.trim();
    command.toLowerCase();
    if (command.startsWith("audio ")) command.remove(0, 6);

    if (command == "tone" || command == "t") {
        playTone(1000);
    } else if (command == "longtone" || command == "l") {
        playTone(3000);
    } else if (command == "cute" || command == "voxie_cute") {
        playPattern("cute", kCute, sizeof(kCute) / sizeof(kCute[0]));
    } else if (command == "yes") {
        playPattern("yes", kYes, sizeof(kYes) / sizeof(kYes[0]));
    } else if (command == "no") {
        playPattern("no", kNo, sizeof(kNo) / sizeof(kNo[0]));
    } else if (command == "scared") {
        playPattern("scared", kScared, sizeof(kScared) / sizeof(kScared[0]));
    } else if (command == "shy") {
        playPattern("shy", kShy, sizeof(kShy) / sizeof(kShy[0]));
    } else if (command == "wake" || command == "wakeup") {
        playPattern("wakeup", kWakeup, sizeof(kWakeup) / sizeof(kWakeup[0]));
    } else if (command == "sleep" || command == "sleepy" || command == "voxie_sleepy") {
        playPattern("sleepy", kSleepy, sizeof(kSleepy) / sizeof(kSleepy[0]));
    } else if (command == "angry" || command == "voxie_angry") {
        playPattern("angry", kAngry, sizeof(kAngry) / sizeof(kAngry[0]));
    } else if (command == "pintest") {
        runPinTest();
    } else if (command == "status") {
        Serial.printf("[audio] ready=yes I2S1 BCLK=%d LRC=%d DIN=%d rate=%lu volume=%u%%\n",
                      kAmpBclk, kAmpLrc, kAmpDin, (unsigned long)kSampleRate,
                      volumePercent);
    } else if (command.startsWith("vol ")) {
        if (!setVolume(command.substring(4))) {
            Serial.println("[audio] usage: audio vol 0-100");
        }
    } else if (command.length()) {
        Serial.printf("[audio] unsupported test command '%s'\n", command.c_str());
    }
}
}  // namespace

void setup() {
    Serial.begin(230400);
    delay(300);
    Serial.println("\nMAX98357A direct-I2S test");
    Serial.printf("[audio] BCLK=%d LRC=%d DIN=%d\n",
                  kAmpBclk, kAmpLrc, kAmpDin);

    if (!startI2s()) {
        Serial.println("[audio] setup failed");
        return;
    }

    Serial.println("[audio] ready; sounds, tone, longtone, pintest, vol 0-100, status");
    playTone(1000);
}

void loop() {
    static String command;
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            handleCommand(command);
            command = "";
        } else if (command.length() < 48) {
            command += ch;
        }
    }
    delay(1);
}
#endif
