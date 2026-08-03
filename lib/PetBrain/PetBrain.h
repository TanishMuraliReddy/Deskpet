#pragma once
#include <Arduino.h>
#include "RoboFace.h"
#include "PetMind.h"

// Owns the desk pet's moment-to-moment behavior. Every frame it picks one
// expression to show based on a fixed priority order - being tipped over
// beats being startled, which beats being touched, which beats answering a
// question, which beats idle fidgeting/asleep - so simultaneous events
// (e.g. a loud noise while answering a question) resolve predictably
// instead of fighting over the display. Being tipped shows DISGRUNTLED
// (escalating to ANGRY if shaken repeatedly); a loud noise shows ANGRY if
// the pet was asleep at that moment, or SCARED otherwise.
//
// While otherwise idle-awake, PetBrain doesn't just sit at NEUTRAL - it
// randomly fidgets into brief HAPPY/SUSPICIOUS/SLEEPY flickers, weighted by
// PetMind's affection/grump levels, so a well-treated pet idles a little
// happier and a frequently shaken/scolded one idles a little warier.
class PetBrain {
public:
    PetBrain(RoboFace& face, PetMind& mind);

    void begin();
    void update(); // call every loop after feeding in this frame's sensor events

    void notifyTouched();
    void notifyTipped(bool isTipped); // call every loop with the current reading
    void notifyLoudNoise();
    void notifySpeak(const PetReply& reply); // starts an "answering" reaction

    // Wake-word reaction (see WakeWordDetector) - an edge, like
    // notifyTouched(): wakes the pet and shows a brief alert "it heard you"
    // look. Ranks above jamming (a wake word interrupts a jam) but below
    // being tipped/startled/touched/answering.
    void notifyWakeWord();

    // Music/rhythm reaction (see BeatDetector). notifyMusicPlaying() is a
    // continuous state, like notifyTipped() - call every loop with the
    // current detection. notifyMusicBeat() is an edge - call once per
    // confirmed beat. Ranks below being touched/tipped/startled/answering,
    // but above idle fidgeting, so a real event still interrupts a jam.
    void notifyMusicPlaying(bool playing);
    void notifyMusicBeat();

    // Serial/UI preview: immediately hold an expression for a short review
    // period, then return to normal sensor-driven behavior.
    void previewExpression(Expression expression, uint32_t durationMs = 10000);

    // Chat-kind replies (greetings, praise, name, stats, ...): show the face
    // with a short caption underneath.
    bool isAnswering() const;
    const String& currentMessage() const;

    // Data-kind replies (weather/time/date): the face briefly closes its
    // eyes (see notifySpeak()) then the screen switches to a clean,
    // full-screen value instead - no face, no long sentence.
    bool isShowingDataScreen() const;
    bool isDataAnswer() const { return _isDataAnswer; } // true even during the brief pre-reveal blink
    const String& dataValue() const;
    const String& dataCaption() const;

    // Lets main.cpp pick a "waking up" sound instead of an event-specific one
    // when a touch/tip happens to be what wakes the pet - check this before
    // calling notifyTouched()/notifyTipped() for the frame, since those calls
    // clear the asleep state themselves.
    bool isAsleep() const { return _asleep; }

    // True while showing any timed reaction (touched/tipped/noise/wake-word/
    // answering/jamming) rather than idle fidgeting or asleep - main.cpp
    // watches this edge to report "back to normal" for test/debug purposes.
    bool isReacting() const;

    // Human-readable name of whatever's currently being shown, for test/
    // debug status lines - not used by the face itself.
    const char* currentStateName() const;

private:
    RoboFace& _face;
    PetMind& _mind;

    uint32_t _lastActivityMs;
    bool _asleep;

    // Idle fidget state
    uint32_t _nextFidgetAtMs;
    uint32_t _fidgetUntilMs;

    bool _tipped;
    uint32_t _lastTipEventMs;
    uint8_t _recentTipCount;

    // Loud-noise reaction: which expression to show is decided once, at the
    // moment the noise fires, based on what the pet was doing right before
    // it (see notifyLoudNoise()) - ANGRY if it was asleep, SCARED otherwise.
    uint32_t _noiseUntilMs;
    Expression _noiseExpr;

    uint32_t _touchedUntilMs;

    uint32_t _attentiveUntilMs;  // wake-word reaction window

    // Jamming: mouth pulses open on each beat (via setMouthOverride, the
    // same hook meant for driving the mouth off a real audio envelope),
    // snapping back shut between beats via _mouthPulseUntilMs.
    bool _jamming;
    uint32_t _mouthPulseUntilMs;
    uint32_t _musicBeatUntilMs;
    Expression _previewExpression;
    uint32_t _previewUntilMs;
    // Answering (chat or data). _dataRevealAtMs is when the blink-close
    // finishes and the data screen may actually appear; for chat replies
    // it equals _answerUntilMs's start (shows immediately).
    uint32_t _answerUntilMs;
    uint32_t _dataRevealAtMs;
    bool _isDataAnswer;
    String _message;
    String _dataValue;
    String _dataCaption;

    Expression _lastApplied;
    bool _hasApplied;

    static const uint32_t kIdleTimeoutMs = 30000;
    static const uint32_t kNoiseReactionMs = 2200;
    static const uint32_t kTouchedMs = 2000;
    static const uint32_t kAttentiveMs = 1800;
    static const uint32_t kTipEventWindowMs = 4000;
    static const uint32_t kDataRevealDelayMs = 220; // lets the eyes visibly finish closing first
    void applyExpression(Expression e, float transitionSeconds);
    void wake();
    void updateFidget(uint32_t now);
};
