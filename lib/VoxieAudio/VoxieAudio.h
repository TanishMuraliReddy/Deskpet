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
    void playCute(); void playNo(); void playScared();
    void playShy(); void playWakeup(); void playYes();
    void playVoxieCute(); void playVoxieAngry(); void playVoxieSleepy();
private:
    int _bclkPin, _lrcPin, _dinPin, _ampEnablePin;
    bool _ready = false, _playing = false;
    bool _outputRunning = false;
    uint8_t _volumePercent = 100;
    const char* _initError = "audio startup has not run";
    bool startOutput();
    void silenceAndStopOutput();
    void setAmpEnabled(bool enabled);
    void playRobotSound(RobotSound sound);
};
