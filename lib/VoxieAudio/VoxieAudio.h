#pragma once
#include <Arduino.h>

class VoxieAudio {
public:
    VoxieAudio(int bclkPin, int lrcPin, int dinPin);
    void begin();
    void update();
    bool isPlaying() const { return _playing; }
    void playCute(); void playNo(); void playScared();
    void playShy(); void playWakeup(); void playYes();
private:
    int _bclkPin, _lrcPin, _dinPin;
    bool _ready = false, _playing = false;
    size_t _offset = 0, _end = 0;
    void play(size_t offset, size_t size);
};
