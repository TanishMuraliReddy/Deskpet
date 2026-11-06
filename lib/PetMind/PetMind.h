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
//
// Adaptive mood, on top of the counters. Touch, voice, praise/scolding,
// being tipped and loud noises nudge a set of moods; update() then lets them
// drift over time like a real pet's would:
//   happiness   relaxes toward its own baseline, and that baseline slowly
//               follows how happy the pet has actually been lately - so a
//               well-treated pet settles at a higher resting mood and a
//               neglected/shaken one at a lower one (the baseline persists).
//   energy      drains while awake, recovers while asleep; low energy means
//               dozing off sooner and sleepier idling.
//   bond        (= affectionLevel()) grows with regular interaction, fades
//               very slowly without it.
//   loneliness  builds after a couple of minutes with no interaction and
//               pulls happiness down; the next touch is an "I missed you".
//   annoyance   short-term: repeated touches in quick succession give less
//               joy each time and finally a grumpy "enough!".
// PetBrain reads these to weight idle expressions, pick the touch reaction,
// set the resting face and the sleep timeout.
enum class TouchFeeling : uint8_t { HAPPY, LOVED, MISSED_YOU, ANNOYED };
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
    // recordTouch() also says how this particular touch felt, so PetBrain
    // can react to it.
    TouchFeeling recordTouch();
    void recordTipped();
    void recordLoudNoise();
    // Voice commands from the PC link: "voxie" (called by name), "wake",
    // "sleep", "hey".
    void recordVoice(const String& kind);

    // Time-based mood drift (and throttled saving). Call every loop.
    void update(bool asleep);

    float affectionLevel() const { return _affection; } // 0..1, the long-term bond
    float grumpLevel() const { return _grump; }          // 0..1
    float happiness() const { return _happy; }           // 0..1, right now
    float happyBaseline() const { return _happyBase; }   // 0..1, where happiness settles
    float energy() const { return _energy; }             // 0..1
    float annoyance() const { return _annoy; }           // 0..1, short-term
    float loneliness() const;                            // 0..1, from time since last interaction
    uint32_t sleepTimeoutMs() const;                     // idle time before dozing off, by energy

    // One-line "[mood] ..." summary for Serial (also parsed by tools/mic_display.py).
    void printMood() const;
    void resetMood();

private:
    Preferences _prefs;

    String _userName;
    float _affection = 0.5f;
    float _grump = 0.2f;
    uint32_t _touchCount = 0;
    uint32_t _tipCount = 0;
    uint32_t _noiseCount = 0;
    uint32_t _commandCount = 0;

    float _happy = 0.6f;
    float _happyBase = 0.55f;
    float _energy = 1.0f;
    float _annoy = 0.0f;
    uint32_t _lastInteractionMs = 0;
    uint32_t _lastUpdateMs = 0;

    bool _dirty = false;
    uint32_t _lastSaveMs = 0;

    void markDirty();
    void maybeSave();
    void save();

    bool tryExtractName(const String& lower, const String& original);
    void interacted();   // someone paid attention: clears loneliness
    void nudgeHappy(float amount);
    static float clamp01(float v);
};
