#pragma once
#include <Arduino.h>

// Wraps the desk pet's senses. Touch is read for real from an ESP32
// touch-capable GPIO (works with just a wire - no sensor module needed).
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
              uint16_t touchThreshold = 40);

    void begin();
    void update(); // call once per loop(), before reading any of the below

    bool touchEdge() const;  // true for the one update() where touch begins
    bool tippedOver() const; // true for as long as the pet is upside down
    bool loudNoiseEdge();    // true once, the update() after a noise event

    // Ignore the microphone briefly while the pet itself is using its
    // speaker, preventing the amp from triggering its own loud-noise loop.
    void suppressLoudNoise(uint32_t durationMs);

    // Live mic amplitude and adaptive floor, exposed for Serial calibration -
    // see the 'm' debug toggle in src/main.cpp.
    int32_t lastPeak() const { return _lastPeak; }
    float noiseFloor() const { return _noiseFloor; }

    // The raw I2S samples behind lastPeak(), for other consumers that need
    // the actual waveform rather than just its peak (BeatDetector only
    // needs the peak; WakeWordDetector needs real samples). This is the
    // same underlying I2S_NUM_0 read update() already does each loop - a
    // second, independent i2s_read() call site would just split the DMA
    // queue's data between the two readers instead of both seeing the full
    // stream, so anything else that wants mic audio reads it from here.
    const int32_t* rawSamples() const { return _rawBuf; }
    size_t rawSampleCount() const { return _rawSampleCount; }

    // Test hook standing in for real IMU hardware.
    void simulateToggleTip();
    // Kept for manual testing over Serial even with a real mic wired up.
    void simulateLoudNoise();

private:
    uint8_t _touchPin;
    uint16_t _touchThreshold;
    bool _touchActive;
    bool _touchEdge;

    bool _simulatedTipped;

    int _micSckPin, _micWsPin, _micSdPin;
    bool _micReady;
    float _noiseFloor;       // slow-moving ambient level estimate
    int32_t _lastPeak;       // most recent chunk's peak abs sample, for calibration
    uint32_t _noiseCooldownUntilMs;
    bool _pendingLoudNoise;

    static const size_t kMicReadSamples = 256;
    int32_t _rawBuf[kMicReadSamples];
    size_t _rawSampleCount = 0;  // samples actually filled in _rawBuf this update(), 0 if none new

    void updateMic();
};
