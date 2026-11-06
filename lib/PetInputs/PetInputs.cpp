#include "PetInputs.h"
#include <driver/i2s.h>

namespace {
constexpr uint32_t kMicSampleRate = 16000;
// Samples are normalized to signed 16-bit PCM below.  Keep these values in
// that same scale: normal room noise is typically in the hundreds to low
// thousands, while a close clap/shout is several thousand or more.
constexpr float kNoiseFactor = 2.0f;         // a spike must beat floor * this to count
constexpr int32_t kMinAbsThreshold = 6000;   // ignores ordinary speech/room noise
constexpr float kAsleepNoiseFactor = 3.5f;
constexpr int32_t kAsleepMinAbsThreshold = 14000;
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
    Serial.printf("[touch] GPIO%d ready; starting threshold %u, then learned from the pad. "
                  "Raw values print every 250 ms.\n", _touchPin, (unsigned)_touchThreshold);

    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = kMicSampleRate,
        // INMP441 delivers 24-bit signed data left-justified in a 32-bit
        // frame. updateMic() reduces that to conventional 16-bit PCM.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // verified against the Super Mini's mic channel
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
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
        return;
    }
    _micQueue = xStreamBufferCreate(kMicQueueSamples * sizeof(int32_t), sizeof(int32_t));
    // Core 0, away from loop() on core 1, so mic capture never waits on it.
    xTaskCreatePinnedToCore(micTask, "mic", 4096, this, 5, nullptr, 0);
}

void PetInputs::micTask(void* self) {
    PetInputs* inputs = static_cast<PetInputs*>(self);
    static int32_t block[kMicTaskSamples];
    while (true) {
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_0, block, sizeof(block), &bytesRead, portMAX_DELAY);
        if (bytesRead == 0) continue;
        if (inputs->_micTap) inputs->_micTap(block, bytesRead / sizeof(int32_t));
        // Non-blocking: if loop() has fallen far behind, drop rather than stall capture.
        xStreamBufferSend(inputs->_micQueue, block, bytesRead, 0);
    }
}

void PetInputs::update() {
    const uint32_t reading = touchRead(_touchPin);
    const uint32_t now = millis();
    // On this ESP32-S3 board and copper-tape pad, touch raises the raw count.
    // Untouched readings are about 20k; two real touches measured 40k+. Both
    // drift with the pad, wiring, humidity and who's touching, so instead of
    // a fixed threshold the pad's resting level and a typical touch's rise
    // above it are learned as it runs. The constructor's threshold only
    // seeds the first guess.
    if (_touchBaseline <= 0.0f) {
        _touchBaseline = reading;
        _touchStrength = max(kMinTouchRise * 2.0f, 2.0f * ((float)_touchThreshold - reading));
    }
    const float pressAt = _touchBaseline + max(kMinTouchRise, 0.5f * _touchStrength);
    const float releaseAt = _touchBaseline + max(kMinTouchRise * 0.6f, 0.3f * _touchStrength);
    bool active = _touchActive ? reading > releaseAt : reading > pressAt;  // hysteresis

    if (active) {
        if (!_touchActive) { _touchStartMs = now; _touchPeak = 0; }
        _touchPeak = max(_touchPeak, reading);
        // Held "on" far longer than any touch: something is resting on the
        // pad or the reading shifted. Treat that as the new resting level.
        if (now - _touchStartMs > kStuckTouchMs) {
            _touchBaseline = reading;
            active = false;
            Serial.printf("[touch] stuck high for %lus - re-learned resting level %u\n",
                          (unsigned long)(kStuckTouchMs / 1000), reading);
        }
    } else if (_touchActive && _touchPeak > _touchBaseline) {
        // Just released: learn how strong a real touch is on this pad.
        _touchStrength = 0.7f * _touchStrength + 0.3f * (_touchPeak - _touchBaseline);
    } else if (reading < _touchBaseline + 0.25f * _touchStrength) {
        // Idle: follow slow drift in the resting level.
        _touchBaseline += (reading - _touchBaseline) * 0.005f;
    }
    _touchThreshold = (uint32_t)(_touchBaseline + max(kMinTouchRise, 0.5f * _touchStrength));

    _touchEdge = active && !_touchActive;
    const bool released = !active && _touchActive;
    _touchActive = active;

    if (_touchEdge) {
        Serial.printf("[touch] TOUCHED  raw=%u  threshold=%u\n", reading, (unsigned)_touchThreshold);
    } else if (released) {
        Serial.printf("[touch] released raw=%u  peak=%u  learned rise=%.0f\n", reading,
                      (unsigned)_touchPeak, _touchStrength);
    } else if (now - _lastTouchTelemetryMs >= 250) {
        Serial.printf("[touch] raw=%u  rest=%.0f  threshold=%u  state=%s\n", reading, _touchBaseline,
                      (unsigned)_touchThreshold, active ? "TOUCHED" : "idle");
        _lastTouchTelemetryMs = now;
    }

    updateMic();
}

void PetInputs::updateMic() {
    if (!_micReady) { _rawSampleCount = 0; return; }

    // Never blocks the main loop: just takes whatever the mic task queued
    // since the last poll.
    const size_t bytesRead = xStreamBufferReceive(_micQueue, _rawBuf, sizeof(_rawBuf), 0);
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

    const float threshold = _asleep
        ? max((float)kAsleepMinAbsThreshold, _noiseFloor * kAsleepNoiseFactor)
        : max((float)kMinAbsThreshold, _noiseFloor * kNoiseFactor);
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
    if (until > _selfSoundUntilMs) _selfSoundUntilMs = until;
}

void PetInputs::simulateToggleTip() { _simulatedTipped = !_simulatedTipped; }

void PetInputs::simulateLoudNoise() { _pendingLoudNoise = true; }
