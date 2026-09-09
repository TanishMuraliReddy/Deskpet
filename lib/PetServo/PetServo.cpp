#include "PetServo.h"
#include <esp_system.h>

namespace {
// Stay comfortably inside the MG90S's physical travel.  The mechanical horn
// or a 3D-printed linkage can bind long before the advertised 180 degrees;
// limiting command travel is quieter and much kinder to the gears.
constexpr int kServoMinAngle = 48;
constexpr int kServoMaxAngle = 132;
constexpr int kPulseMinUs = 500;
constexpr int kPulseMaxUs = 2400;
// Do not keep re-sending imperceptibly different pulse widths, but retain
// much finer resolution than Servo::write(angle)'s ~10.5 us per degree.
constexpr int kPulseDeadbandUs = 3;
// Feed each point of a trajectory at the same 50 Hz cadence as the servo
// pulse.  The old 25 Hz / two-degree filter made a small MG90S visibly step
// between positions instead of gliding through them.
constexpr uint32_t kServoFrameMs = 20;
constexpr int kRequestDeadbandDegrees = 1;
constexpr uint32_t kSettleHoldMs = 420;
// The OLED's eyes make frequent short saccades. A physical MG90S should not
// chase each one: its gears will buzz and it looks nervous. Body glances are
// therefore intentionally infrequent, slow, and small.
// The face makes a new deliberate saccade roughly every 0.9–1.9 seconds.
// Match that rhythm so the body reads as following the eyes, rather than
// making an unrelated occasional movement.
constexpr uint32_t kMinEyeFollowIntervalMs = 850;
constexpr int kEyeTurnRangeDegrees = 22;

float easeInOut(float t) {
    t = constrain(t, 0.0f, 1.0f);
    return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2) * 0.5f;
}

int angleToPulseUs(int angle) {
    const int safeAngle = constrain(angle, kServoMinAngle, kServoMaxAngle);
    return kPulseMinUs + (safeAngle * (kPulseMaxUs - kPulseMinUs)) / 180;
}
} // namespace

PetServo::PetServo(uint8_t signalPin) : _pin(signalPin) {}

void PetServo::begin() {
    randomSeed(esp_random());
    // Reserve the PWM timers before attaching the servo.  ESP32Servo uses
    // hardware PWM (MCPWM on this S3 where available, LEDC as its fallback),
    // so I2S audio and other foreground work cannot stretch the 50 Hz pulse.
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    _servo.setPeriodHertz(50);
    // ESP32Servo assigns the first working servo to PWM channel 0. Channel
    // zero is valid, so use attached() rather than treating the return value
    // as a boolean (the old > 0 check silently disabled the first servo).
    _servo.attach(_pin, kPulseMinUs, kPulseMaxUs);
    _ready = _servo.attached();
    if (!_ready) {
        Serial.printf("[servo] failed to attach GPIO%d\n", _pin);
        return;
    }
    _hasWritten = false;
    writeAngle(kNeutralAngle, true);
    // Briefly establish the neutral pose, then release the PWM signal. An
    // analog MG90S otherwise continues correcting tiny position errors and
    // produces the familiar idle buzz/jitter.
    _detachAtMs = millis() + kSettleHoldMs;
    Serial.printf("[servo] GPIO%d MG90S quiet-rest enabled at %d degrees\n", _pin, kNeutralAngle);
}

bool PetServo::attachIfNeeded() {
    if (!_ready) return false;
    if (_servo.attached()) return true;

    _servo.attach(_pin, kPulseMinUs, kPulseMaxUs);
    if (!_servo.attached()) {
        Serial.printf("[servo] failed to reattach GPIO%d\n", _pin);
        _ready = false;
        return false;
    }

    // Reassert the last known pose before changing direction.  This avoids a
    // sudden first-frame jump when the PWM generator wakes up.
    _servo.writeMicroseconds(_currentPulseUs);
    _hasWritten = true;
    _lastFrameMs = 0;
    return true;
}

void PetServo::writeAngle(int angle, bool force) {
    const int nextAngle = constrain(angle, kServoMinAngle, kServoMaxAngle);
    writePulseUs(angleToPulseUs(nextAngle), force);
    if (_hasWritten) _currentAngle = nextAngle;
}

void PetServo::writePulseUs(int pulseUs, bool force) {
    const int nextPulseUs = constrain(pulseUs, kPulseMinUs, kPulseMaxUs);
    if (!attachIfNeeded()) return;
    if (!_hasWritten || force || abs(nextPulseUs - _currentPulseUs) >= kPulseDeadbandUs) {
        _servo.writeMicroseconds(nextPulseUs);
        _currentPulseUs = nextPulseUs;
        _hasWritten = true;
    }
}

void PetServo::startSequence(const Step* steps, uint8_t count) {
    if (!_ready || count == 0) return;
    if (!attachIfNeeded()) return;

    const int firstTarget = constrain(steps[0].angle, kServoMinAngle, kServoMaxAngle);
    if (count == 1 && abs(firstTarget - _currentAngle) <= kRequestDeadbandDegrees) {
        _detachAtMs = millis() + kSettleHoldMs;
        return;
    }
    _stepCount = min(count, kMaxSteps);
    for (uint8_t i = 0; i < _stepCount; ++i) _steps[i] = steps[i];
    _stepIndex = 0;
    _fromAngle = _currentAngle;
    _fromPulseUs = _currentPulseUs;
    _stepStartedMs = millis();
    _detachAtMs = 0; // an active sequence must retain holding torque
}

void PetServo::update() {
    if (!_ready) return;

    const uint32_t now = millis();
    if (_stepIndex >= _stepCount) {
        releaseWhenSettled();
        return;
    }

    // Update position once per servo frame. Updating at the ESP32's much
    // faster loop rate makes a small analog MG90 chase PWM changes that it
    // cannot physically follow and sounds/looks jittery.
    if (now - _lastFrameMs < kServoFrameMs) return;
    _lastFrameMs = now;

    const Step& step = _steps[_stepIndex];
    const uint32_t elapsed = now - _stepStartedMs;
    const float t = step.durationMs == 0 ? 1.0f : min(1.0f, elapsed / (float)step.durationMs);
    const int targetPulseUs = angleToPulseUs(step.angle);
    writePulseUs((int)roundf(_fromPulseUs + (targetPulseUs - _fromPulseUs) * easeInOut(t)),
                 t >= 1.0f);

    if (t < 1.0f) return;
    _fromAngle = constrain(step.angle, kServoMinAngle, kServoMaxAngle);
    _currentAngle = _fromAngle;
    _fromPulseUs = _currentPulseUs;
    ++_stepIndex;
    _stepStartedMs = now;
    if (_stepIndex >= _stepCount) _detachAtMs = now + kSettleHoldMs;
}

void PetServo::center() {
    if (abs(_currentAngle - kNeutralAngle) <= 1) {
        _detachAtMs = millis();
        return;
    }
    static const Step motion[] = {{kNeutralAngle, 180}};
    startSequence(motion, 1);
}

void PetServo::happyWag() {
    static const Step motion[] = {{110, 850}, {70, 850}, {108, 700}, {kNeutralAngle, 900}};
    startSequence(motion, sizeof(motion) / sizeof(motion[0]));
}

void PetServo::angryShake() {
    // Keep the reaction readable without repeatedly slamming the gears at
    // their reversal points.
    static const Step motion[] = {{112, 600}, {68, 580}, {112, 580}, {68, 580}, {kNeutralAngle, 750}};
    startSequence(motion, sizeof(motion) / sizeof(motion[0]));
}

void PetServo::startledTwitch() {
    static const Step motion[] = {{80, 550}, {100, 600}, {kNeutralAngle, 800}};
    startSequence(motion, sizeof(motion) / sizeof(motion[0]));
}

void PetServo::wakeNod() {
    static const Step motion[] = {{72, 750}, {kNeutralAngle, 900}};
    startSequence(motion, sizeof(motion) / sizeof(motion[0]));
}

void PetServo::vibeStep(bool leftBeat) {
    static const Step left[] = {{80, 450}, {kNeutralAngle, 600}};
    static const Step right[] = {{100, 450}, {kNeutralAngle, 600}};
    startSequence(leftBeat ? left : right, 2);
}

void PetServo::diagnosticSweep() {
    // Slow, wide travel: removes any ambiguity caused by the pet's subtle
    // normal movements while still staying clear of common MG90S end stops.
    static const Step motion[] = {
        {kServoMinAngle, 1200}, {kServoMaxAngle, 1800},
        {kServoMinAngle, 1800}, {kNeutralAngle, 1200}
    };
    startSequence(motion, sizeof(motion) / sizeof(motion[0]));
}

void PetServo::followEyes(float gazeX, float gazeY, bool awake) {
    if (!_ready) return;

    if (!awake) {
        if (_eyeTrackingEnabled) center();
        _eyeTrackingEnabled = false;
        return;
    }
    _eyeTrackingEnabled = true;

    // Event motions (touch, scare, vibe, etc.) take priority.  This keeps
    // reactions readable and prevents the idle behavior from fighting them.
    if (isMoving()) return;

    const uint32_t now = millis();
    const float clampedX = constrain(gazeX, -1.0f, 1.0f);
    const float clampedY = constrain(gazeY, -1.0f, 1.0f);
    const float gazeChange = fabsf(clampedX - _lastGazeX) +
                             fabsf(clampedY - _lastGazeY) * 0.35f;
    if (now - _lastEyeFollowMs < kMinEyeFollowIntervalMs || gazeChange < 0.55f) return;

    _lastEyeFollowMs = now;
    _lastGazeX = clampedX;
    _lastGazeY = clampedY;

    // One servo axis cannot literally reproduce vertical eye motion. Map a
    // vertical glance to a small extra tilt, but let horizontal gaze produce
    // an obvious 22-degree body turn.  Hold the turn briefly so it lands with
    // the rendered eyes before smoothly returning to neutral.
    const int target = kNeutralAngle +
                       (int)roundf(clampedX * kEyeTurnRangeDegrees + clampedY * 3.0f);
    if (abs(target - kNeutralAngle) < 8) return;

    Step glance[] = {
        {target, (uint16_t)random(800, 1050)},
        {target, (uint16_t)random(400, 600)},
        {kNeutralAngle, (uint16_t)random(900, 1150)},
    };
    startSequence(glance, sizeof(glance) / sizeof(glance[0]));
}

bool PetServo::isMoving() const {
    return _ready && _stepIndex < _stepCount;
}

bool PetServo::isAttached() {
    return _ready && _servo.attached();
}

void PetServo::releaseWhenSettled() {
    if (!_servo.attached() || _detachAtMs == 0 || millis() < _detachAtMs) return;
    _servo.detach();
    // Detaching stops the LEDC/PWM peripheral. Explicitly making the pin an
    // input with a pulldown prevents a floating signal wire from looking like
    // random pulses to the MG90S while it is parked.
    pinMode(_pin, INPUT_PULLDOWN);
    _hasWritten = false;
    _detachAtMs = 0;
    Serial.println("[servo] quiet rest (PWM released)");
}
