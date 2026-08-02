#pragma once
#include <Arduino.h>

// Gives the desk pet a voice through a MAX98357A I2S amplifier + speaker.
// Like RoboFace's eyes, sounds are synthesized live (simple frequency-swept
// tone bursts) rather than played back from stored audio clips, so there are
// no sound assets to keep on flash. Runs on its own I2S port (I2S_NUM_1),
// separate from the microphone's (I2S_NUM_0), so playback never contends
// with the mic for the I2S peripheral/sample rate - see PetInputs.
class PetVoice {
public:
    PetVoice(int bclkPin, int lrcPin, int dinPin);

    void begin();

    // One reactive stinger per event PetBrain already distinguishes. Each
    // call blocks for its short duration (all well under 200ms) since these
    // only ever fire on rare discrete events, not every frame.
    void playTouched();   // short soft upward chirp - happy
    void playTipped();    // short low grumble - disgruntled/angry
    void playStartled();  // sharp rising yelp - scared/angry-from-noise
    void playWake();      // gentle short blip - waking from sleep
    void playTestTone();  // one-second 440 Hz tone for wiring/volume checks

private:
    int _bclkPin, _lrcPin, _dinPin;
    bool _ready;
    static const uint32_t kSampleRate = 16000;

    // Synthesizes and writes one tone sweep (startHz -> endHz over
    // durationMs) with a short fade in/out so it doesn't click.
    void playSweep(float startHz, float endHz, uint32_t durationMs, float volume);
};
