#pragma once
#include <Arduino.h>
#include <Preferences.h>

// A fully local, hardcoded stand-in for "the pet understands you." There is
// no language model here - a real one needs far more RAM/flash/compute than
// an ESP32 has. Instead: weighted keyword/synonym matching picks an intent,
// several response templates are chosen from at random per intent so it
// doesn't sound robotic, simple pattern matching extracts and remembers
// your name, and running affection/grump counters shift with how you treat
// the pet (touches, being tipped over, praise, scolding) - persisted to
// flash so they survive a reboot. PetBrain reads affectionLevel()/
// grumpLevel() to bias its idle "fidget" expressions, so a well-treated pet
// idles a little happier and a frequently shaken/scolded one idles a little
// warier. That's "learning" as accumulated, deterministic counters - not a
// trained model - but it does mean the pet's baseline mood genuinely
// depends on how it's been used over time.
// Weather/time/date answers are a single glanceable fact, so PetBrain shows
// them full-screen and clean (just the value, e.g. "72F") instead of
// cluttering the face with a sentence. Conversational replies (greetings,
// praise, name, stats, ...) keep the face on screen with a short caption.
struct PetReply {
    String spoken;    // full sentence - always set, printed to Serial
    bool isData;      // true for weather/time/date
    String value;     // set only if isData, e.g. "72F", "03:45 PM"
    String caption;   // set only if isData and there's a short qualifier, e.g. "partly cloudy"
};

class PetMind {
public:
    void begin(); // loads persisted memory from flash (NVS)

    // Classifies text (typed in as a stand-in for recognized speech - see
    // VoiceCommands' old role, now folded in here) and returns a reply.
    PetReply respond(const String& text);

    // Rising-edge events PetBrain forwards here to update counters/mood.
    void recordTouch();
    void recordTipped();
    void recordLoudNoise();

    float affectionLevel() const { return _affection; } // 0..1
    float grumpLevel() const { return _grump; }          // 0..1

private:
    Preferences _prefs;

    String _userName;
    float _affection = 0.5f;
    float _grump = 0.2f;
    uint32_t _touchCount = 0;
    uint32_t _tipCount = 0;
    uint32_t _noiseCount = 0;
    uint32_t _commandCount = 0;

    bool _dirty = false;
    uint32_t _lastSaveMs = 0;

    void markDirty();
    void maybeSave();
    void save();

    bool tryExtractName(const String& lower, const String& original);
    static float clamp01(float v);
};
