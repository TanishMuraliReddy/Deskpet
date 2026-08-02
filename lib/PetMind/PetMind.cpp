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
    _dirty = false;
    _lastSaveMs = millis();
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
    _dirty = false;
    _lastSaveMs = millis();
}

void PetMind::recordTouch() {
    _touchCount++;
    _affection = clamp01(_affection + 0.05f);
    _grump = clamp01(_grump - 0.02f);
    markDirty();
    maybeSave();
}

void PetMind::recordTipped() {
    _tipCount++;
    _grump = clamp01(_grump + 0.08f);
    _affection = clamp01(_affection - 0.03f);
    markDirty();
    maybeSave();
}

void PetMind::recordLoudNoise() {
    _noiseCount++;
    _grump = clamp01(_grump + 0.02f);
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
        maybeSave();
        return chatReply(pickTemplate(kPraiseTemplates, arrayLen(kPraiseTemplates), _userName));
    }
    if (containsAny(lower, kScoldWords, arrayLen(kScoldWords))) {
        _grump = clamp01(_grump + 0.10f);
        _affection = clamp01(_affection - 0.05f);
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
        if (_affection - _grump > 0.3f) return chatReply("I'm feeling great, especially around you.");
        if (_grump - _affection > 0.3f) return chatReply("Honestly? A little grumpy right now.");
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
