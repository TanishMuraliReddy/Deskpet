#pragma once
#include <Arduino.h>
#include <freertos/stream_buffer.h>

// Wraps the desk pet's senses. Touch is read for real from an ESP32
// touch-capable GPIO (works with just a wire - no sensor module needed),
// with the pad's resting level and touch strength learned as it runs.
// Loud-noise detection is also real now, via an I2S microphone (INMP441):
// PetInputs runs its own adaptive noise floor over the mic's live amplitude
// and raises loudNoiseEdge() the moment a sample burst spikes well above
// recent ambient level - see updateMic() in the .cpp for the tuning knobs.
// Tilt (orientation) still needs hardware this project doesn't have yet (an
// IMU like an MPU6050), so it stays simulated: call simulateToggleTip() from
// wherever you want to fake that event for now (src/main.cpp does this from
// Serial). PetBrain only ever sees tippedOver()/touchEdge()/loudNoiseEdge(),
// so swapping the tilt simulation for a real IMU later means editing this
// file only - nothing else in the project needs to change.
class PetInputs {
public:
    // micSckPin/micWsPin/micSdPin wire to an INMP441 (or similar) I2S mic's
    // SCK/WS/SD pins; its L/R pin should be tied to GND (selects the left
    // channel, which is what this class reads).
    PetInputs(uint8_t touchPin, int micSckPin, int micWsPin, int micSdPin,
              uint16_t touchThreshold = 33000);

    void begin();
    void update(); // call once per loop(), before reading any of the below
    void setAsleep(bool asleep) { _asleep = asleep; }

    bool touchEdge() const;  // true for the one update() where touch begins
    bool tippedOver() const; // true for as long as the pet is upside down
    bool loudNoiseEdge();    // true once, the update() after a noise event

    // Ignore the microphone briefly while the pet itself is using its
    // speaker, preventing the amp from triggering its own loud-noise loop.
    void suppressLoudNoise(uint32_t durationMs);
    // True while inside such a window - the mic is (probably) hearing the
    // pet's own speaker, so music/beat detection should skip it too.
    bool hearingSelf() const { return millis() < _selfSoundUntilMs; }

    // Live mic amplitude and adaptive floor, exposed for Serial calibration -
    // see the 'm' debug toggle in src/main.cpp.
    int32_t lastPeak() const { return _lastPeak; }
    float noiseFloor() const { return _noiseFloor; }

    // The raw I2S samples behind lastPeak(), for other consumers that need
    // the actual waveform rather than just its peak (BeatDetector only
    // needs the peak; WakeWordDetector needs real samples). These are the
    // samples update() drained this loop - a second, independent i2s_read()
    // call site would just split the DMA queue's data between the two
    // readers, so anything else that wants mic audio reads it from here.
    const int32_t* rawSamples() const { return _rawBuf; }
    size_t rawSampleCount() const { return _rawSampleCount; }

    // Called from the mic task (core 0) with every block straight off I2S,
    // independent of how slow loop() is - for the PC voice stream, which
    // must not lose audio while the OLED or a sound render holds up loop().
    // Keep it short and thread-safe; set it before begin().
    using MicTap = void (*)(const int32_t* samples, size_t count);
    void setMicTap(MicTap tap) { _micTap = tap; }

    // Test hook standing in for real IMU hardware.
    void simulateToggleTip();
    // Kept for manual testing over Serial even with a real mic wired up.
    void simulateLoudNoise();

private:
    uint8_t _touchPin;
    uint32_t _touchThreshold;    // current press threshold; seeded by the constructor, then learned
    bool _touchActive;
    bool _touchEdge;
    uint32_t _lastTouchTelemetryMs = 0;
    float _touchBaseline = 0.0f; // learned untouched reading
    float _touchStrength = 0.0f; // learned rise of a real touch above the baseline
    uint32_t _touchPeak = 0;
    uint32_t _touchStartMs = 0;
    static constexpr float kMinTouchRise = 2000.0f;    // never trigger on less than this
    static const uint32_t kStuckTouchMs = 15000;

    bool _simulatedTipped;

    int _micSckPin, _micWsPin, _micSdPin;
    bool _micReady;
    bool _asleep = false;
    float _noiseFloor;       // slow-moving ambient level estimate
    int32_t _lastPeak;       // most recent chunk's peak abs sample, for calibration
    uint32_t _noiseCooldownUntilMs;
    bool _pendingLoudNoise;
    uint32_t _selfSoundUntilMs = 0;

    // A dedicated task does the blocking I2S reads and queues the samples
    // here; update() drains up to kMicReadSamples of them per loop.
    static const size_t kMicReadSamples = 1024;
    static const size_t kMicTaskSamples = 256;
    static const size_t kMicQueueSamples = 4096;  // ~250 ms of slack for a slow loop()
    int32_t _rawBuf[kMicReadSamples];
    size_t _rawSampleCount = 0;  // samples actually filled in _rawBuf this update(), 0 if none new
    StreamBufferHandle_t _micQueue = nullptr;
    MicTap _micTap = nullptr;

    void updateMic();
    static void micTask(void* self);
};
