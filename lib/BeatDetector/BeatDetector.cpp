#include "BeatDetector.h"
#include <math.h>

namespace {
constexpr float kOnsetFactor = 1.35f;         // works with music whose peaks are not extreme
constexpr float kFloorAlpha = 0.02f;          // preserve a slower background envelope
constexpr float kMaxCoeffVariation = 0.30f;   // real music timing is less exact than a metronome
constexpr uint32_t kMinIntervalMs = 240;      // ~250 BPM ceiling
constexpr uint32_t kMaxIntervalMs = 1400;     // ~43 BPM floor
}  // namespace

void BeatDetector::reset() {
    _avgEnergy = 500.0f;
    _lastOnsetMs = 0;
    _intervalCount = 0;
    _intervalIdx = 0;
    _musicDetected = false;
    _beatEdge = false;
    _bpm = 0.0f;
}

void BeatDetector::update(int32_t amplitude) {
    _beatEdge = false;
    uint32_t now = millis();
    float level = (float)amplitude;

    bool isOnset = level > _avgEnergy * kOnsetFactor &&
                   (now - _lastOnsetMs) >= kMinOnsetGapMs;

    // The rolling average always adapts (same fix as PetInputs' noise
    // floor) so it tracks the room's actual loudness instead of freezing
    // the moment music starts - a single sample's pull is capped so one
    // onset doesn't drag the average up to meet it.
    float capped = fminf(level, _avgEnergy * kOnsetFactor);
    _avgEnergy = _avgEnergy * (1.0f - kFloorAlpha) + capped * kFloorAlpha;

    if (isOnset) {
        recordOnset(now);
    } else if (_musicDetected && (now - _lastOnsetMs) > kSilenceTimeoutMs) {
        _musicDetected = false;
        _intervalCount = 0;
        _bpm = 0.0f;
    }
}

void BeatDetector::recordOnset(uint32_t now) {
    if (_lastOnsetMs != 0) {
        uint32_t interval = now - _lastOnsetMs;
        if (interval >= kMinIntervalMs && interval <= kMaxIntervalMs) {
            _intervals[_intervalIdx] = interval;
            _intervalIdx = (_intervalIdx + 1) % kIntervalHistory;
            if (_intervalCount < kIntervalHistory) _intervalCount++;
            evaluateRhythm();
        } else {
            // Way outside any plausible tempo range - not part of the same
            // beat pattern, so start a fresh interval history from here.
            _intervalCount = 0;
            _intervalIdx = 0;
            _musicDetected = false;
            _bpm = 0.0f;
        }
    }
    _lastOnsetMs = now;
}

void BeatDetector::evaluateRhythm() {
    if (_intervalCount < kMinOnsetsForMusic) return;

    // Coefficient of variation (stddev/mean) rather than a plain min-max
    // spread: a single stray onset can't throw the whole check off the way
    // it could with a range-based check, so this holds up much better
    // against room noise that only occasionally happens to land two beats
    // apart by chance.
    float sum = 0;
    for (uint8_t i = 0; i < _intervalCount; i++) sum += _intervals[i];
    float mean = sum / _intervalCount;

    float variance = 0;
    for (uint8_t i = 0; i < _intervalCount; i++) {
        float diff = _intervals[i] - mean;
        variance += diff * diff;
    }
    variance /= _intervalCount;
    float coeffVariation = sqrtf(variance) / mean;

    if (coeffVariation <= kMaxCoeffVariation) {
        _musicDetected = true;
        _bpm = 60000.0f / mean;
        _beatEdge = true;
    } else if (!_musicDetected) {
        // Never leave an old tentative tempo in the debug output after a
        // sequence turns out to be irregular noise rather than rhythm.
        _bpm = 0.0f;
    }
}
