#include "PetMind.h"
#include "WeatherService.h"
#include "TimeService.h"

namespace {

bool containsAny(const String& lower, const char* const* words, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (lower.indexOf(words[i]) >= 0) return true;
    }
    return false;
}

String pickTemplate(const char* const* templates, uint8_t count, const String& name) {
    String out = templates[random(0, count)];
    out.replace("{name}", name.length() ? name : "friend");
    return out;
}

PetReply chatReply(const String& spoken) { return {spoken, false, "", ""}; }

// clang-format off
const char* const kNamePatterns[]   = {"my name is ", "call me ", "i am ", "i'm "};
const char* const kNameQueryWords[] = {"my name", "who am i"};
const char* const kStatsWords[]     = {"how many times", "stats", "remember about me", "what do you know"};
const char* const kPraiseWords[]    = {"good boy", "good girl", "good pet", "i love you", "love you",
                                       "cute", "adorable", "well done", "good job", "proud of you"};
const char* const kScoldWords[]     = {"bad boy", "bad girl", "bad pet", "stupid", "dumb",
                                       "shut up", "i hate", "annoying", "bad robot"};
const char* const kGreetingWords[]  = {"hello", "hey", "good morning", "good evening", "howdy"};
const char* const kIdentityWords[]  = {"who are you", "what are you", "are you a robot", "are you alive"};
const char* const kMoodWords[]      = {"how are you", "how do you feel", "are you happy", "are you sad"};
const char* const kWeatherWords[]   = {"weather", "temperature", "outside", "raining", "sunny"};
const char* const kTimeWords[]      = {"time", "clock", "o'clock"};
const char* const kDateWords[]      = {"date", "day is it", "today"};

const char* const kNameAckTemplates[]  = {"Nice to meet you, {name}!", "Got it - you're {name}.", "{name}. I like it."};
const char* const kPraiseTemplates[]   = {"Aw, thanks {name}!", "You're making me blush.", "That means a lot, {name}."};
const char* const kScoldTemplates[]    = {"Ok, ok, I'm sorry.", "That's a bit harsh, {name}...", "Noted. I'll do better."};
const char* const kGreetingTemplates[] = {"Hey {name}!", "Hello there!", "Hi! Good to see you."};
const char* const kIdentityTemplates[] = {"I'm your desk pet - part robot, part vibes.",
                                          "Just a friendly little display with opinions.",
                                          "Whatever you programmed me to be, {name}."};
const char* const kUnknownTemplates[]  = {"Sorry, I don't know that one yet.", "Hmm, not sure what you mean.",
                                          "I didn't quite catch that."};
// clang-format on

template <typename T>
constexpr uint8_t arrayLen(const T& arr) {
    return sizeof(arr) / sizeof(arr[0]);
}

// Mood dynamics (all times in seconds).
constexpr float kHappyRelaxS = 120.0f;       // happiness settles back to baseline over ~2 min
constexpr float kBaselineFollowS = 3600.0f;  // baseline follows actual happiness over ~1 h
constexpr float kBaselineMin = 0.2f, kBaselineMax = 0.85f;
constexpr float kEnergyDrainS = 1800.0f;     // full -> empty awake in ~30 min
constexpr float kEnergyRecoverS = 300.0f;    // empty -> full asleep in ~5 min
constexpr float kAnnoyDecayS = 20.0f;
constexpr float kGrumpDecayS = 1800.0f;      // grump fades toward kGrumpRest over ~30 min
constexpr float kGrumpRest = 0.1f;
constexpr float kBondDecayS = 6.0f * 3600.0f;  // bond fades toward kBondRest over hours
constexpr float kBondRest = 0.3f;
constexpr float kLonelyAfterS = 120.0f;      // no interaction for 2 min starts loneliness...
constexpr float kLonelyFullS = 600.0f;       // ...full after another 10
constexpr float kLonelySadS = 600.0f;        // full loneliness drains happiness at this rate
constexpr uint32_t kSleepMinMs = 15000, kSleepMaxMs = 120000;

} // namespace

float PetMind::clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void PetMind::begin() {
    _prefs.begin("petmind", false);
    _userName = _prefs.getString("name", "");
    _affection = _prefs.getFloat("affection", 0.5f);
    _grump = _prefs.getFloat("grump", 0.2f);
    _touchCount = _prefs.getUInt("touch", 0);
    _tipCount = _prefs.getUInt("tip", 0);
    _noiseCount = _prefs.getUInt("noise", 0);
    _commandCount = _prefs.getUInt("cmds", 0);
    _happyBase = _prefs.getFloat("happyBase", 0.55f);
    _happy = _prefs.getFloat("happy", _happyBase);
    _energy = _prefs.getFloat("energy", 1.0f);
    _dirty = false;
    _lastSaveMs = millis();
    _lastInteractionMs = _lastUpdateMs = millis();
}

float PetMind::loneliness() const {
    const float idleS = (millis() - _lastInteractionMs) / 1000.0f;
    return clamp01((idleS - kLonelyAfterS) / kLonelyFullS);
}

uint32_t PetMind::sleepTimeoutMs() const {
    return kSleepMinMs + (uint32_t)(_energy * (kSleepMaxMs - kSleepMinMs));
}

void PetMind::interacted() { _lastInteractionMs = millis(); }

void PetMind::nudgeHappy(float amount) {
    _happy = clamp01(_happy + amount);
    markDirty();
}

void PetMind::update(bool asleep) {
    const uint32_t now = millis();
    if (now - _lastUpdateMs < 1000) return;
    const float dt = (now - _lastUpdateMs) / 1000.0f;
    _lastUpdateMs = now;

    _happy += (_happyBase - _happy) * min(1.0f, dt / kHappyRelaxS);
    if (!asleep) {
        // Asleep it isn't missing anyone, and a night left alone shouldn't
        // teach it to be miserable - both only run while it's awake.
        _happy -= loneliness() * dt / kLonelySadS;
        // The adaptive part: where happiness settles is itself learned from
        // how the pet has actually been feeling.
        _happyBase += (clamp01(_happy) - _happyBase) * min(1.0f, dt / kBaselineFollowS);
        _happyBase = constrain(_happyBase, kBaselineMin, kBaselineMax);
    }
    _happy = clamp01(_happy);

    _energy = clamp01(_energy + (asleep ? dt / kEnergyRecoverS : -dt / kEnergyDrainS));
    _annoy *= expf(-dt / kAnnoyDecayS);
    _grump += (kGrumpRest - _grump) * min(1.0f, dt / kGrumpDecayS);
    _affection += (kBondRest - _affection) * min(1.0f, dt / kBondDecayS);
    // Slow drift alone only needs saving now and then (flash wear); events
    // mark the state dirty and go through the usual 30 s throttle.
    if (now - _lastSaveMs > 300000) save();
    else maybeSave();
}

void PetMind::printMood() const {
    Serial.printf("[mood] happy=%.2f base=%.2f energy=%.2f bond=%.2f grump=%.2f lonely=%.2f annoy=%.2f\n",
                  _happy, _happyBase, _energy, _affection, _grump, loneliness(), _annoy);
}

void PetMind::resetMood() {
    _happyBase = 0.55f;
    _happy = _happyBase;
    _energy = 1.0f;
    _affection = 0.5f;
    _grump = 0.2f;
    _annoy = 0.0f;
    interacted();
    save();
}

void PetMind::markDirty() { _dirty = true; }

void PetMind::maybeSave() {
    // Flash (NVS) writes are throttled rather than done on every touch/tip,
    // since those can fire in quick bursts and flash wear is finite.
    if (_dirty && millis() - _lastSaveMs > 30000) save();
}

void PetMind::save() {
    _prefs.putString("name", _userName);
    _prefs.putFloat("affection", _affection);
    _prefs.putFloat("grump", _grump);
    _prefs.putUInt("touch", _touchCount);
    _prefs.putUInt("tip", _tipCount);
    _prefs.putUInt("noise", _noiseCount);
    _prefs.putUInt("cmds", _commandCount);
    _prefs.putFloat("happyBase", _happyBase);
    _prefs.putFloat("happy", _happy);
    _prefs.putFloat("energy", _energy);
    _dirty = false;
    _lastSaveMs = millis();
}

TouchFeeling PetMind::recordTouch() {
    _touchCount++;
    const float lonely = loneliness();
    // Habituation: each touch in a quick burst is worth less than the last.
    const float novelty = 1.0f - _annoy;
    TouchFeeling feeling;
    if (_annoy > 0.75f) {
        feeling = TouchFeeling::ANNOYED;
        nudgeHappy(-0.03f);
        _grump = clamp01(_grump + 0.03f);
    } else {
        nudgeHappy(0.12f * novelty * (0.6f + _affection) + 0.15f * lonely);
        _affection = clamp01(_affection + 0.02f * novelty);
        _grump = clamp01(_grump - 0.02f);
        feeling = lonely > 0.3f ? TouchFeeling::MISSED_YOU
                : _affection > 0.6f ? TouchFeeling::LOVED : TouchFeeling::HAPPY;
    }
    _annoy = clamp01(_annoy + 0.22f);
    _energy = clamp01(_energy + 0.03f);
    interacted();
    markDirty();
    maybeSave();
    return feeling;
}

void PetMind::recordTipped() {
    _tipCount++;
    _grump = clamp01(_grump + 0.08f);
    _affection = clamp01(_affection - 0.03f);
    _annoy = clamp01(_annoy + 0.3f);
    nudgeHappy(-0.15f);
    interacted();
    markDirty();
    maybeSave();
}

void PetMind::recordLoudNoise() {
    _noiseCount++;
    _grump = clamp01(_grump + 0.02f);
    nudgeHappy(-0.05f);
    markDirty();
    maybeSave();
}

void PetMind::recordVoice(const String& kind) {
    if (kind == "voxie") {
        // Being called by name is attention in itself.
        nudgeHappy(0.04f);
        _affection = clamp01(_affection + 0.01f);
    } else if (kind == "hey") {
        nudgeHappy(0.08f + 0.1f * loneliness());
        _affection = clamp01(_affection + 0.01f);
    } else if (kind == "wake") {
        _energy = clamp01(_energy + 0.15f);
    }
    interacted();
    markDirty();
    maybeSave();
}

bool PetMind::tryExtractName(const String& lower, const String& original) {
    for (uint8_t i = 0; i < arrayLen(kNamePatterns); i++) {
        int idx = lower.indexOf(kNamePatterns[i]);
        if (idx < 0) continue;

        int start = idx + (int)strlen(kNamePatterns[i]);
        int end = original.indexOf(' ', start);
        if (end < 0) end = original.length();
        for (int p = start; p < end; p++) {
            char c = original.charAt(p);
            if (c == '.' || c == ',' || c == '!' || c == '?') { end = p; break; }
        }

        String name = original.substring(start, end);
        name.trim();
        if (name.length() == 0) continue;

        name.setCharAt(0, (char)toupper(name.charAt(0)));
        _userName = name;
        return true;
    }
    return false;
}

PetReply PetMind::respond(const String& textIn) {
    String original = textIn;
    original.trim();
    String lower = original;
    lower.toLowerCase();

    _commandCount++;
    interacted();
    markDirty();

    if (tryExtractName(lower, original)) {
        maybeSave();
        return chatReply(pickTemplate(kNameAckTemplates, arrayLen(kNameAckTemplates), _userName));
    }
    if (containsAny(lower, kNameQueryWords, arrayLen(kNameQueryWords))) {
        return chatReply(_userName.length() ? ("You're " + _userName + ", of course.")
                                             : "You haven't told me your name yet.");
    }
    if (containsAny(lower, kStatsWords, arrayLen(kStatsWords))) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "You've touched me %u times, tipped me %u times, and asked me %u things.",
                 (unsigned)_touchCount, (unsigned)_tipCount, (unsigned)_commandCount);
        return chatReply(String(buf));
    }
    if (containsAny(lower, kPraiseWords, arrayLen(kPraiseWords))) {
        _affection = clamp01(_affection + 0.10f);
        _grump = clamp01(_grump - 0.05f);
        nudgeHappy(0.12f);
        maybeSave();
        return chatReply(pickTemplate(kPraiseTemplates, arrayLen(kPraiseTemplates), _userName));
    }
    if (containsAny(lower, kScoldWords, arrayLen(kScoldWords))) {
        _grump = clamp01(_grump + 0.10f);
        _affection = clamp01(_affection - 0.05f);
        nudgeHappy(-0.15f);
        maybeSave();
        return chatReply(pickTemplate(kScoldTemplates, arrayLen(kScoldTemplates), _userName));
    }
    if (containsAny(lower, kGreetingWords, arrayLen(kGreetingWords))) {
        return chatReply(pickTemplate(kGreetingTemplates, arrayLen(kGreetingTemplates), _userName));
    }
    if (containsAny(lower, kIdentityWords, arrayLen(kIdentityWords))) {
        return chatReply(pickTemplate(kIdentityTemplates, arrayLen(kIdentityTemplates), _userName));
    }
    if (containsAny(lower, kMoodWords, arrayLen(kMoodWords))) {
        if (_energy < 0.2f) return chatReply("Sleepy... so sleepy.");
        if (loneliness() > 0.5f) return chatReply("A bit lonely, honestly. Glad you're here.");
        if (_happy > 0.7f) return chatReply("I'm feeling great, especially around you.");
        if (_grump > 0.5f || _happy < 0.3f) return chatReply("Honestly? A little grumpy right now.");
        return chatReply("I'm doing okay!");
    }
    // Weather/time/date are a single glanceable fact, so a successful fetch
    // becomes a full-screen data reply (see PetBrain's isShowingDataScreen())
    // instead of a sentence under the face. A failure (no WiFi, not synced
    // yet, ...) still reads as the pet talking to you, so that stays chat.
    if (containsAny(lower, kWeatherWords, arrayLen(kWeatherWords))) {
        WeatherInfo w = fetchWeather();
        if (!w.ok) return chatReply(w.sentence);
        return {w.sentence, true, w.value, w.caption};
    }
    if (containsAny(lower, kTimeWords, arrayLen(kTimeWords))) {
        TimeInfo t = fetchTime();
        if (!t.ok) return chatReply(t.sentence);
        return {t.sentence, true, t.value, ""};
    }
    if (containsAny(lower, kDateWords, arrayLen(kDateWords))) {
        TimeInfo d = fetchDate();
        if (!d.ok) return chatReply(d.sentence);
        return {d.sentence, true, d.value, ""};
    }

    maybeSave();
    return chatReply(pickTemplate(kUnknownTemplates, arrayLen(kUnknownTemplates), _userName));
}
