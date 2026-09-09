#pragma once

#include <Arduino.h>
#include <ESP32Servo.h>

// Non-blocking body language for one MG90S-style 50 Hz hobby servo. Every
// motion finishes at 90 degrees and its PWM is detached after settling, so a
// resting desk pet is silent instead of continuously hunting for position.
class PetServo {
public:
    static constexpr int kNeutralAngle = 90;

    explicit PetServo(uint8_t signalPin);

    void begin();
    void update();

    void happyWag();       // gentle acknowledgement for touch/chat
    void angryShake();     // quick right-left protest
    void startledTwitch(); // short recoil for an irregular loud sound
    void wakeNod();        // slow wake-up motion
    void vibeStep(bool leftBeat);
    void diagnosticSweep(); // slow full-range power/signal check
    void center();

    // Local, state-aware behavior layer. It follows clear eye glances with a
    // small delayed body turn, ignores micro-movements, and chooses varied
    // timing so it feels intentional rather than metronomic. `awake=false`
    // returns it to neutral and releases PWM once settled.
    void followEyes(float gazeX, float gazeY, bool awake);

    bool isMoving() const;
    bool isAttached();

private:
    struct Step {
        int angle;
        uint16_t durationMs;
    };

    static constexpr uint8_t kMaxSteps = 8;
    Servo _servo;
    uint8_t _pin;
    bool _ready = false;
    int _currentAngle = kNeutralAngle;
    int _fromAngle = kNeutralAngle;
    int _currentPulseUs = 1450;
    int _fromPulseUs = 1450;
    Step _steps[kMaxSteps]{};
    uint8_t _stepCount = 0;
    uint8_t _stepIndex = 0;
    uint32_t _stepStartedMs = 0;
    uint32_t _lastFrameMs = 0;
    uint32_t _detachAtMs = 0;
    uint32_t _lastEyeFollowMs = 0;
    bool _hasWritten = false;
    bool _eyeTrackingEnabled = false;
    float _lastGazeX = 0.0f;
    float _lastGazeY = 0.0f;

    void startSequence(const Step* steps, uint8_t count);
    bool attachIfNeeded();
    void writeAngle(int angle, bool force = false);
    void writePulseUs(int pulseUs, bool force = false);
    void releaseWhenSettled();
};
