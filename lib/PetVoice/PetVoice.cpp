#include "PetVoice.h"
#include <driver/i2s.h>
#include <math.h>

PetVoice::PetVoice(int bclkPin, int lrcPin, int dinPin)
    : _bclkPin(bclkPin), _lrcPin(lrcPin), _dinPin(dinPin), _ready(false) {}

void PetVoice::begin() {
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = kSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = _bclkPin,
        .ws_io_num = _lrcPin,
        .data_out_num = _dinPin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    _ready = i2s_driver_install(I2S_NUM_1, &config, 0, nullptr) == ESP_OK &&
             i2s_set_pin(I2S_NUM_1, &pins) == ESP_OK;
    if (!_ready) {
        Serial.println("PetVoice: I2S amp init failed - check BCLK/LRC/DIN wiring.");
    }
}

void PetVoice::playSweep(float startHz, float endHz, uint32_t durationMs, float volume) {
    if (!_ready) return;

    uint32_t totalSamples = kSampleRate * durationMs / 1000;
    // Fade over the first/last 8% so bursts don't click at the edges.
    uint32_t fadeSamples = max((uint32_t)8, totalSamples / 12);
    float phase = 0.0f;

    const uint32_t kChunk = 128;
    int16_t stereoBuf[kChunk * 2];

    for (uint32_t base = 0; base < totalSamples; base += kChunk) {
        uint32_t n = min(kChunk, totalSamples - base);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t sampleIdx = base + i;
            float t = (float)sampleIdx / (float)totalSamples;
            float freq = startHz + (endHz - startHz) * t;
            phase += 2.0f * PI * freq / (float)kSampleRate;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;

            float env = 1.0f;
            if (sampleIdx < fadeSamples) env = (float)sampleIdx / (float)fadeSamples;
            else if (sampleIdx > totalSamples - fadeSamples)
                env = (float)(totalSamples - sampleIdx) / (float)fadeSamples;

            int16_t sample = (int16_t)(sinf(phase) * volume * env * 32000.0f);
            // MAX98357A's SD pin is left disconnected (mixes L+R by default -
            // see the wiring notes), so the same sample goes to both slots.
            stereoBuf[i * 2] = sample;
            stereoBuf[i * 2 + 1] = sample;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_1, stereoBuf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    }
}

void PetVoice::playTouched() { playSweep(700.0f, 1100.0f, 110, 0.5f); }
void PetVoice::playTipped() { playSweep(220.0f, 140.0f, 160, 0.6f); }
void PetVoice::playStartled() { playSweep(500.0f, 1600.0f, 140, 0.75f); }
void PetVoice::playWake() { playSweep(500.0f, 800.0f, 90, 0.4f); }
void PetVoice::playTestTone() { playSweep(440.0f, 440.0f, 1000, 0.5f); }
