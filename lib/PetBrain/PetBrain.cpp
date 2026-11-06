#include "PetBrain.h"

PetBrain::PetBrain(RoboFace& face, PetMind& mind)
    : _face(face), _mind(mind),
      _lastActivityMs(0), _asleep(false),
      _nextFidgetAtMs(0), _fidgetUntilMs(0),
      _tipped(false), _lastTipEventMs(0), _recentTipCount(0),
      _noiseUntilMs(0), _noiseExpr(Expression::SCARED),
      _touchedUntilMs(0),
      _attentiveUntilMs(0),
      _jamming(false), _mouthPulseUntilMs(0), _musicBeatUntilMs(0),
      _previewExpression(Expression::NEUTRAL), _previewUntilMs(0),
      _answerUntilMs(0), _dataRevealAtMs(0), _isDataAnswer(false),
      _lastApplied(Expression::NEUTRAL), _hasApplied(false) {}

void PetBrain::begin() {
    _lastActivityMs = millis();
    applyExpression(Expression::NEUTRAL, 0.01f);
}

void PetBrain::applyExpression(Expression e, float transitionSeconds) {
    if (!_hasApplied || e != _lastApplied) {
        _face.setExpression(e, transitionSeconds);
        _lastApplied = e;
        _hasApplied = true;
    }
}

void PetBrain::wake() {
    _sleepRequested = false;
    if (_asleep) {
        _asleep = false;
        _face.blinkOnce();
    }
}

TouchFeeling PetBrain::notifyTouched() {
    const bool wasAsleep = _asleep;
    wake();
    uint32_t duration = kTouchedMs;
    const TouchFeeling feeling = _mind.recordTouch();
    switch (feeling) {
        case TouchFeeling::ANNOYED:    _touchedExpr = Expression::DISGRUNTLED; break;
        case TouchFeeling::HAPPY:      _touchedExpr = Expression::HAPPY; break;
        case TouchFeeling::LOVED:      _touchedExpr = Expression::ADORING; break;
        case TouchFeeling::MISSED_YOU: _touchedExpr = Expression::ADORING; duration = kMissedYouMs; break;
    }
    // Woken up while still worn out: groggy rather than delighted.
    if (wasAsleep && _mind.energy() < 0.25f && _touchedExpr != Expression::DISGRUNTLED) {
        _touchedExpr = Expression::SLEEPY;
    }
    _touchedUntilMs = millis() + duration;
    _lastActivityMs = millis();
    return feeling;
}

void PetBrain::notifyTipped(bool isTipped) {
    uint32_t now = millis();
    if (isTipped && !_tipped) {
        // Getting tipped several times in a short window reads as being
        // shaken, which escalates the reaction from disgruntled to angry.
        _recentTipCount = (now - _lastTipEventMs < kTipEventWindowMs) ? _recentTipCount + 1 : 1;
        _lastTipEventMs = now;
        wake();
        _mind.recordTipped();
    }
    _tipped = isTipped;
    if (isTipped) _lastActivityMs = now;
}

void PetBrain::notifyLoudNoise() {
    // A noise while asleep reads as being startled awake and annoyed about
    // it (angry); the same noise while already awake reads as a fright
    // (scared). Decided once here, from state as of just before wake().
    _noiseExpr = _asleep ? Expression::ANGRY : Expression::SCARED;
    wake();
    _mind.recordLoudNoise();
    _noiseUntilMs = millis() + kNoiseReactionMs;
    _lastActivityMs = millis();
}

void PetBrain::notifyWakeWord() {
    wake();
    _attentiveUntilMs = millis() + kAttentiveMs;
    _lastActivityMs = millis();
}

void PetBrain::notifyGoToSleep() {
    const uint32_t now = millis();
    _touchedUntilMs = _answerUntilMs = _attentiveUntilMs = _previewUntilMs = 0;
    _noiseUntilMs = 0;
    _fidgetUntilMs = 0;
    _lastActivityMs = now;
    _sleepRequested = true;  // update() takes the asleep branch next frame
}

void PetBrain::notifySpeak(const PetReply& reply) {
    uint32_t now = millis();
    bool wasAsleep = _asleep;
    _isDataAnswer = reply.isData;
    _message = reply.spoken;
    _dataValue = reply.value;
    _dataCaption = reply.caption;

    // Data replies read as a clean instrument readout, so the eyes close
    // first (like a blink) and the value only appears once that finishes -
    // a beat later, not an instant cut. Chat replies show immediately, same
    // as before.
    if (reply.isData) {
        _face.blinkOnce();
        _dataRevealAtMs = now + (wasAsleep ? 0 : kDataRevealDelayMs);
    } else {
        _dataRevealAtMs = now;
    }
    wake();

    uint32_t textForTiming = reply.isData ? reply.value.length() : reply.spoken.length();
    uint32_t duration = (uint32_t)constrain((int)(textForTiming * 70), 1500, 6000);
    if (reply.isData) duration = max(duration, (uint32_t)2200); // give a beat to actually read the value
    _answerUntilMs = now + duration;
    _lastActivityMs = now;
}

void PetBrain::notifyMusicPlaying(bool playing) {
    if (playing && !_jamming) wake();
    if (playing) _lastActivityMs = millis();
    _jamming = playing;
}

void PetBrain::notifyMusicBeat() {
    if (!_jamming) return;
    _mouthPulseUntilMs = millis() + 130;
    _musicBeatUntilMs = millis() + 180;
    _lastActivityMs = millis();
}

void PetBrain::previewExpression(Expression expression, uint32_t durationMs) {
    _previewExpression = expression;
    _previewUntilMs = millis() + durationMs;
    _lastActivityMs = millis();
}

bool PetBrain::isAnswering() const { return millis() < _answerUntilMs; }

const String& PetBrain::currentMessage() const { return _message; }

bool PetBrain::isShowingDataScreen() const {
    uint32_t now = millis();
    if (_tipped || now < _noiseUntilMs || now < _touchedUntilMs) return false;
    return _isDataAnswer && now >= _dataRevealAtMs && now < _answerUntilMs;
}

const String& PetBrain::dataValue() const { return _dataValue; }
const String& PetBrain::dataCaption() const { return _dataCaption; }

void PetBrain::update() {
    uint32_t now = millis();
    _mind.update(_asleep);
    _face.setTalking(false);
    // Defaults off every frame; the jamming branch below re-asserts it when
    // that's actually this frame's state, so a higher-priority reaction
    // (tipped/noise/touched/answering) preempting a jam can't leave the
    // mouth stuck mid-pulse instead of showing that reaction's own mouth.
    _face.setMouthOverride(false);

    if (now < _previewUntilMs) {
        applyExpression(_previewExpression, 0.01f);
        return;
    }

    if (_tipped) {
        applyExpression(_recentTipCount >= 3 ? Expression::ANGRY : Expression::DISGRUNTLED, 0.25f);
        return;
    }
    if (now < _noiseUntilMs) {
        applyExpression(_noiseExpr, 0.15f);
        return;
    }
    if (now < _touchedUntilMs) {
        // A physical touch should feel immediate; keep a short blend so it
        // still reads as organic rather than as a hard screen cut.
        applyExpression(_touchedExpr, 0.08f);
        return;
    }
    if (now < _answerUntilMs) {
        applyExpression(Expression::HAPPY, 0.3f);
        _face.setTalking(true);
        return;
    }
    if (now < _attentiveUntilMs) {
        // Called while worn out: a drowsy "hm?" instead of bright attention.
        applyExpression(_mind.energy() < 0.25f ? Expression::SLEEPY : Expression::SURPRISED, 0.12f);
        return;
    }
    if (_jamming) {
        applyExpression(now < _musicBeatUntilMs ? Expression::VIBING_ACTIVE : Expression::VIBING, 0.10f);
        bool mouthOpen = now < _mouthPulseUntilMs;
        _face.setMouthOverride(true, mouthOpen ? 0.85f : 0.05f);
        return;
    }

    // A tired pet dozes off sooner (PetMind scales this with energy).
    if (_sleepRequested || now - _lastActivityMs > _mind.sleepTimeoutMs()) {
        _asleep = true;
        _fidgetUntilMs = 0;
        applyExpression(Expression::SLEEPING, 1.2f);
    } else {
        updateFidget(now);
    }
}

bool PetBrain::isReacting() const {
    uint32_t now = millis();
    return _tipped || now < _noiseUntilMs || now < _touchedUntilMs ||
           now < _answerUntilMs || now < _attentiveUntilMs || _jamming;
}

const char* PetBrain::currentStateName() const {
    uint32_t now = millis();
    if (_tipped) return _recentTipCount >= 3 ? "angry (shaken)" : "disgruntled (tipped)";
    if (now < _noiseUntilMs) return _noiseExpr == Expression::ANGRY ? "angry (startled awake)" : "scared (loud noise)";
    if (now < _touchedUntilMs) return "adoring (touched)";
    if (now < _answerUntilMs) return "talking (answering)";
    if (now < _attentiveUntilMs) return "surprised (wake word)";
    if (_jamming) return "jamming (music)";
    if (_asleep) return "asleep";
    return "idle";
}

Expression PetBrain::restingExpression() const {
    // The face it settles into between fidgets follows its mood baseline:
    // worn out looks sleepy, unhappy or lonely looks sad, otherwise neutral.
    if (_mind.energy() < 0.2f) return Expression::SLEEPY;
    if (_mind.happiness() < 0.3f || _mind.loneliness() > 0.7f) return Expression::SAD;
    return Expression::NEUTRAL;
}

void PetBrain::updateFidget(uint32_t now) {
    if (_fidgetUntilMs != 0 && now < _fidgetUntilMs) {
        return; // mid-fidget; keep showing whatever it rolled into
    }
    if (_fidgetUntilMs != 0 && now >= _fidgetUntilMs) {
        _fidgetUntilMs = 0; // fidget just ended, fall through to reconsider
    }
    if (now < _nextFidgetAtMs) {
        applyExpression(restingExpression(), 0.4f);
        return;
    }

    // Fidget odds follow PetMind's live mood: a happy, well-bonded pet
    // flickers happy/adoring more; a grumpy one glances around warily; a
    // tired one gets heavy-eyed; a lonely or unhappy one looks sad. Most
    // rolls still land on the resting face, so it reads as occasional
    // personality rather than a constant twitch.
    const float happy = _mind.happiness();
    const float lonely = _mind.loneliness();
    const uint8_t happyChance = (uint8_t)(4.0f + happy * 26.0f);
    const uint8_t adoringChance = (uint8_t)(_mind.affectionLevel() > 0.6f ? happy * 10.0f : 0.0f);
    const uint8_t suspiciousChance = (uint8_t)(4.0f + _mind.grumpLevel() * 22.0f);
    const uint8_t sleepyChance = (uint8_t)(2.0f + (1.0f - _mind.energy()) * 22.0f);
    const uint8_t sadChance = (uint8_t)(lonely * 22.0f + max(0.0f, 0.4f - happy) * 30.0f);
    uint8_t roll = (uint8_t)random(0, 100);

    if (roll < happyChance) {
        applyExpression(Expression::HAPPY, 0.4f);
        _fidgetUntilMs = now + 1800;
    } else if ((roll -= happyChance) < adoringChance) {
        applyExpression(Expression::ADORING, 0.4f);
        _fidgetUntilMs = now + 1600;
    } else if ((roll -= adoringChance) < suspiciousChance) {
        applyExpression(Expression::SUSPICIOUS, 0.4f);
        _fidgetUntilMs = now + 1500;
    } else if ((roll -= suspiciousChance) < sleepyChance) {
        applyExpression(Expression::SLEEPY, 0.4f);
        _fidgetUntilMs = now + 1300;
    } else if ((roll -= sleepyChance) < sadChance) {
        applyExpression(Expression::SAD, 0.5f);
        _fidgetUntilMs = now + 2000;
    } else {
        applyExpression(restingExpression(), 0.4f);
    }

    // A tired pet fidgets less often; a lively one more.
    const float pace = 1.6f - _mind.energy();  // 0.6x .. 1.6x the base gap
    _nextFidgetAtMs = now + (uint32_t)(random(4000, 12000) * pace);
}
