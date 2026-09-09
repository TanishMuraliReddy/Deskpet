#include "PetInputs.h"
#include <driver/i2s.h>

namespace {
constexpr uint32_t kMicSampleRate = 16000;
// Samples are normalized to signed 16-bit PCM below.  Keep these values in
// that same scale: normal room noise is typically in the hundreds to low
// thousands, while a close clap/shout is several thousand or more.
constexpr float kNoiseFactor = 2.0f;         // a spike must beat floor * this to count
constexpr int32_t kMinAbsThreshold = 6000;   // ignores ordinary speech/room noise
constexpr uint32_t kNoiseCooldownMs = 900;   // ignore further spikes while one event is still ringing
constexpr float kFloorAlpha = 0.02f;         // how fast the ambient estimate adapts
} // namespace

PetInputs::PetInputs(uint8_t touchPin, int micSckPin, int micWsPin, int micSdPin,
                      uint16_t touchThreshold)
    : _touchPin(touchPin), _touchThreshold(touchThreshold),
      _touchActive(false), _touchEdge(false),
      _simulatedTipped(false),
      _micSckPin(micSckPin), _micWsPin(micWsPin), _micSdPin(micSdPin),
      _micReady(false), _noiseFloor(2000.0f), _lastPeak(0),
      _noiseCooldownUntilMs(0), _pendingLoudNoise(false) {}

void PetInputs::begin() {
    // touchRead() needs no pinMode() call on the ESP32 Arduino core.
    Serial.printf("[touch] GPIO%d ready; active above %u. Raw values will print every 250 ms.\n",
                  _touchPin, _touchThreshold);

    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = kMicSampleRate,
        // INMP441 delivers 24-bit signed data left-justified in a 32-bit
        // frame. updateMic() reduces that to conventional 16-bit PCM.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // mic's L/R pin ties to GND
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = _micSckPin,
        .ws_io_num = _micWsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = _micSdPin,
    };

    _micReady = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) == ESP_OK &&
                i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
    if (!_micReady) {
        Serial.println("PetInputs: I2S mic init failed - check SD/SCK/WS wiring; loud-noise detection disabled.");
    }
}

void PetInputs::update() {
    uint16_t reading = touchRead(_touchPin);
    // On this ESP32-S3 board and copper-tape pad, touch raises the raw count.
    // Untouched readings are about 20k; two real touches measured 40k+.
    bool active = reading > _touchThreshold;
    _touchEdge = active && !_touchActive;
    const bool released = !active && _touchActive;
    _touchActive = active;

    const uint32_t now = millis();
    if (_touchEdge) {
        Serial.printf("[touch] TOUCHED  raw=%u  threshold=%u\n", reading, _touchThreshold);
    } else if (released) {
        Serial.printf("[touch] released raw=%u\n", reading);
    } else if (now - _lastTouchTelemetryMs >= 250) {
        Serial.printf("[touch] raw=%u  threshold=%u  state=%s\n", reading, _touchThreshold,
                      active ? "TOUCHED" : "idle");
        _lastTouchTelemetryMs = now;
    }

    updateMic();
}

void PetInputs::updateMic() {
    if (!_micReady) { _rawSampleCount = 0; return; }

    size_t bytesRead = 0;
    // ticks_to_wait = 0: never blocks the main loop, just grabs whatever the
    // DMA buffer already has queued since the last poll.
    i2s_read(I2S_NUM_0, _rawBuf, sizeof(_rawBuf), &bytesRead, 0);
    size_t samples = bytesRead / sizeof(int32_t);
    _rawSampleCount = samples;  // exposed via rawSamples()/rawSampleCount() for other mic consumers
    if (samples == 0) return;

    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        // INMP441's 24-bit left-justified frame becomes signed 16-bit PCM
        // by retaining its most significant 16 bits.  The prior >>8 value
        // was 256x larger than the configured detector thresholds, so the
        // adaptive floor and the threshold were in incompatible units.
        int32_t sample = _rawBuf[i] >> 16;
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
    }
    _lastPeak = peak;

    // The floor always adapts, even during a loud event - a single sample's
    // pull on it is capped at kNoiseFactor times the *current* floor, so one
    // bang can't drag the baseline way up in one step, but a sustained
    // change in room volume (talking, music, the AC turning on) still
    // settles the floor to a realistic new baseline within a few seconds.
    // Previously this line only ran when a sample was quiet, which meant
    // once the room got loud enough to cross the threshold even once, the
    // floor froze forever and every subsequent sample kept re-triggering
    // "loud noise" against a baseline that never caught up.
    float cappedForFloor = fminf((float)peak, _noiseFloor * kNoiseFactor);
    _noiseFloor = _noiseFloor * (1.0f - kFloorAlpha) + cappedForFloor * kFloorAlpha;

    uint32_t now = millis();
    if (now < _noiseCooldownUntilMs) return; // still riding out the same loud event

    float threshold = max((float)kMinAbsThreshold, _noiseFloor * kNoiseFactor);
    if ((float)peak > threshold) {
        _pendingLoudNoise = true;
        _noiseCooldownUntilMs = now + kNoiseCooldownMs;
    }
}

bool PetInputs::touchEdge() const { return _touchEdge; }

bool PetInputs::tippedOver() const { return _simulatedTipped; }

bool PetInputs::loudNoiseEdge() {
    bool fired = _pendingLoudNoise;
    _pendingLoudNoise = false;
    return fired;
}

void PetInputs::suppressLoudNoise(uint32_t durationMs) {
    _pendingLoudNoise = false;
    uint32_t until = millis() + durationMs;
    if (until > _noiseCooldownUntilMs) _noiseCooldownUntilMs = until;
}

void PetInputs::simulateToggleTip() { _simulatedTipped = !_simulatedTipped; }

void PetInputs::simulateLoudNoise() { _pendingLoudNoise = true; }
