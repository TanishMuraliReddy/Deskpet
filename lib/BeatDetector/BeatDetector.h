#pragma once
#include <Arduino.h>

// Simple energy-based beat/rhythm detector. Feeds off the same mic
// amplitude PetInputs already samples every loop (see lastPeak()) - no
// second I2S consumer needed. An "onset" fires when the instantaneous level
// spikes well above its own slow-moving rolling average; once a run of
// onsets lands at roughly the same interval apart, that's classified as
// music (rhythmic, sustained) rather than a single knock/spike or the
// irregular pattern of talking, and beatEdge() starts firing in time with
// it. This is a hobbyist-grade onset detector, not real tempo/beat-tracking
// analysis - it won't identify genre or survive syncopation, but it's
// enough to nod along to a steady beat.
class BeatDetector {
public:
    void update(int32_t amplitude); // call every loop with the current mic level
    void reset();                   // discard a rhythm when the signal is not music-loud

    bool beatEdge() const { return _beatEdge; }  // true for the one update() a confirmed beat lands on
    bool isMusicDetected() const { return _musicDetected; }
    float bpm() const { return _bpm; }

private:
    static const uint8_t kIntervalHistory = 8;
    static const uint8_t kMinOnsetsForMusic = 4;
    static const uint32_t kMinOnsetGapMs = 200;      // refractory period, caps detection at 300 BPM
    static const uint32_t kSilenceTimeoutMs = 2500;  // no onset this long -> music stopped

    float _avgEnergy = 500.0f;
    uint32_t _lastOnsetMs = 0;
    uint32_t _intervals[kIntervalHistory] = {0};
    uint8_t _intervalCount = 0;
    uint8_t _intervalIdx = 0;

    bool _musicDetected = false;
    bool _beatEdge = false;
    float _bpm = 0.0f;

    void recordOnset(uint32_t now);
    void evaluateRhythm();
};
