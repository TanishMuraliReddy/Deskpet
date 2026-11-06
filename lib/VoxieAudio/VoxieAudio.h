#pragma once
#include <Arduino.h>

class VoxieAudio {
public:
    enum class RobotSound : uint8_t {
        CUTE, YES, NO, SCARED, SHY, WAKEUP, SLEEPY, ANGRY, TEST_TONE
    };
    // ampEnablePin drives MAX98357A SD_MODE: LOW is hardware shutdown,
    // HIGH enables playback. Pass -1 only when SD_MODE is not wired.
    VoxieAudio(int bclkPin, int lrcPin, int dinPin, int ampEnablePin = -1);
    void begin();
    void update();
    bool isPlaying() const { return _playing; }
    void printStatus() const;
    void printMenu() const;
    bool playNumber(uint8_t number);
    void setVolumePercent(uint8_t percent);
    uint8_t volumePercent() const { return _volumePercent; }
    void playTestTone();
    void playTestMp3();
    void playLongTone();  // 3 s at 440 Hz, for checking wiring by ear
    void runPinTest();    // tries all 6 arrangements of the amp's signal pins
    void playCute(); void playNo(); void playScared();
    void playShy(); void playWakeup(); void playYes();
    void playVoxieCute(); void playVoxieAngry(); void playVoxieSleepy();

    // Mood-colored voice: every sound's pitch and loudness are scaled by
    // these (1.0 = as designed). main.cpp sets them from PetMind each loop,
    // so a tired pet sounds lower and softer, a happy one a bit brighter.
    void setMoodVoice(float pitch, float level);

    // Called just before each sound starts with its length, so the mic side
    // can ignore the pet hearing itself (see main.cpp). Every sound also
    // prints "[audio] playing <name> <ms>ms" for the PC voice window.
    using PlayHook = void (*)(uint32_t durationMs);
    void setPlayHook(PlayHook hook) { _playHook = hook; }
private:
    int _bclkPin, _lrcPin, _dinPin, _ampEnablePin;
    bool _ready = false, _playing = false;
    bool _outputRunning = false;
    uint8_t _volumePercent = 100;
    float _moodPitch = 1.0f, _moodLevel = 1.0f;
    PlayHook _playHook = nullptr;
    const char* _initError = "audio startup has not run";
    bool startI2s(int bclkPin, int lrcPin, int dinPin);
    void writeTone(float hz, uint32_t ms, float level);
    bool startOutput();
    void silenceAndStopOutput();
    void setAmpEnabled(bool enabled);
    void playRobotSound(RobotSound sound);
};
