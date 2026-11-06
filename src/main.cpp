#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Adafruit_GFX.h>
#include "RoboFace.h"
#include "PetBrain.h"
#include "PetInputs.h"
#include "PetMind.h"
#include "BeatDetector.h"
#include "VoxieAudio.h"
#include "PetServo.h"

// 1.5 in SH1107 128x128 OLED, four-wire SPI.
#define OLED_CLK  1
#define OLED_DATA 2
#define OLED_CS   5
#define OLED_DC   4
#define OLED_RST  3
#define PANEL_W   128
#define PANEL_H   128

// INMP441 microphone and touch pad wiring.
#define MIC_SCK 10
#define MIC_WS  7
#define MIC_SD  6
#define TOUCH_PIN 9

// Volume settings buttons (momentary, wired between each GPIO and GND).
#define LEFT_BUTTON_PIN 16
#define RIGHT_BUTTON_PIN 17

// MAX98357A I2S speaker amplifier (separate I2S bus from the microphone).
#define AMP_LRC  12
#define AMP_BCLK 13
#define AMP_DIN  11
// MAX98357A SD / SD_MODE is not wired in the current harness, so the amp
// stays enabled. Set a GPIO here if it is wired later: LOW mutes the amp when
// idle, HIGH enables it only while a sound is being played.
#define AMP_SD   -1

// Standard hobby-servo signal pin. GPIO15 is otherwise unused by the OLED,
// microphone, and amplifier. Servo power must come from a separate 5 V rail.
#define SERVO_PIN 15

// VIBE is intentionally reserved for clearly loud music, not speech or
// quiet household sounds. These values match the microphone's normalized
// peak scale printed by the `m` serial-debug command.
constexpr int32_t kVibeMinimumPeak = 5000;
constexpr int32_t kAsleepVibeMinimumPeak = 14000;
constexpr float kVibeMinimumFloor = 1200.0f;
constexpr uint32_t kButtonDebounceMs = 35;
constexpr uint32_t kSettingsHoldMs = 3000;
constexpr uint32_t kSettingsIdleMs = 30000;
constexpr uint8_t kVolumeStepPercent = 20;

U8G2_SH1107_PIMORONI_128X128_F_4W_SW_SPI oled(
    U8G2_R0, OLED_CLK, OLED_DATA, OLED_CS, OLED_DC, OLED_RST);

class U8g2Gfx : public Adafruit_GFX {
public:
    U8g2Gfx(U8G2* device) : Adafruit_GFX(PANEL_W, PANEL_H), _device(device) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
        _device->setDrawColor(color ? 1 : 0);
        _device->drawPixel(x, y);
    }
private:
    U8G2* _device;
};

U8g2Gfx gfx(&oled);
RoboFace face(&gfx, PANEL_W, PANEL_H);
PetMind mind;
PetBrain brain(face, mind);
PetInputs inputs(TOUCH_PIN, MIC_SCK, MIC_WS, MIC_SD);
BeatDetector beat;
VoxieAudio audio(AMP_BCLK, AMP_LRC, AMP_DIN, AMP_SD);
PetServo servo(SERVO_PIN);
Preferences settingsPreferences;

bool wasTipped = false;
bool wasMusicPlaying = false;
bool danceLeft = false;
bool micDebug = false;
bool settingsMode = false;
bool settingsPreferencesReady = false;
uint32_t settingsLastInputMs = 0;

struct SettingsButton {
    bool rawPressed = false;
    bool stablePressed = false;
    bool pressedInSettings = false;
    bool suppressRelease = false;
    uint32_t rawChangedAtMs = 0;
    uint32_t pressedAtMs = 0;
};

SettingsButton leftSettingsButton;
SettingsButton rightSettingsButton;

// Voice link for tools/mic_display.py: the PC does the speech recognition,
// so "stream on" bumps Serial to kStreamBaud and sends the mic as framed
// packets (0xA5 0x5A, uint16 sample count, int16 LE samples). Regular text
// logs keep flowing between packets. The PC answers with "voice ..." lines.
constexpr uint32_t kNormalBaud = 230400;
constexpr uint32_t kStreamBaud = 921600;
volatile bool voiceStream = false;
uint32_t lastLoudMusicCandidateMs = 0;
uint32_t lastDisplayRefreshMs = 0;

void drawVibeOverlay() {
    // Compact label plus two bouncing note accents. The face itself changes
    // from a calm groove to bright wide eyes on each confirmed beat.
    oled.setDrawColor(1);
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(3, 8, "VIBE");
    const int lift = beat.beatEdge() ? 4 : 0;
    const int leftX = danceLeft ? 10 : 18;
    const int rightX = danceLeft ? 112 : 104;
    oled.drawDisc(leftX, 27 - lift, 3);
    oled.drawVLine(leftX + 3, 14 - lift, 13);
    oled.drawHLine(leftX + 3, 14 - lift, 7);
    oled.drawDisc(rightX, 31 + lift, 3);
    oled.drawVLine(rightX + 3, 18 + lift, 13);
    oled.drawHLine(rightX - 3, 18 + lift, 6);
}

void drawVolumeSettingsScreen(uint32_t now) {
    oled.setDrawColor(1);
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(31, 19, "VOLUME");
    oled.drawHLine(12, 27, 104);

    oled.drawBox(15, 48, 10, 16);
    oled.drawTriangle(25, 48, 37, 40, 37, 72);
    const uint8_t waveCount = ((now / 250) & 1) ? 2 : 1;
    for (uint8_t wave = 0; wave < waveCount; ++wave) {
        const int8_t offset = 4 + wave * 6;
        oled.drawLine(37 + offset, 48 - wave * 4, 37 + offset, 64 + wave * 4);
    }

    char volumeText[8];
    snprintf(volumeText, sizeof(volumeText), "%u%%", audio.volumePercent());
    oled.setFont(u8g2_font_logisoso24_tf);
    oled.drawStr(56, 61, volumeText);

    constexpr int16_t barX = 12;
    constexpr int16_t barY = 82;
    constexpr int16_t barW = 104;
    constexpr int16_t barH = 17;
    constexpr int16_t segmentGap = 3;
    constexpr int16_t segmentW = (barW - segmentGap * 4) / 5;
    oled.drawFrame(barX, barY, barW, barH);
    const uint8_t activeSegments = audio.volumePercent() / kVolumeStepPercent;
    for (uint8_t i = 0; i < 5; ++i) {
        const int16_t x = barX + 2 + i * (segmentW + segmentGap);
        if (i < activeSegments) {
            oled.drawBox(x, barY + 2, segmentW, barH - 4);
        }
    }

    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(12, 111, "LEFT: QUIETER");
    oled.drawStr(67, 111, "RIGHT: LOUDER");
    oled.drawStr(5, 124, "BOTH: EXIT   AUTO-EXIT: 30 SEC");
}

bool previewFace(const String& requested) {
    struct FaceCommand { const char* name; Expression expression; };
    static const FaceCommand faces[] = {
        {"neutral", Expression::NEUTRAL}, {"happy", Expression::HAPPY},
        {"sad", Expression::SAD}, {"angry", Expression::ANGRY},
        {"disgruntled", Expression::DISGRUNTLED}, {"surprised", Expression::SURPRISED},
        {"sleepy", Expression::SLEEPY}, {"sleeping", Expression::SLEEPING},
        {"suspicious", Expression::SUSPICIOUS}, {"adoring", Expression::ADORING},
        {"scared", Expression::SCARED}, {"vibe", Expression::VIBING},
        {"vibe_active", Expression::VIBING_ACTIVE},
    };
    String name = requested;
    name.trim();
    name.toLowerCase();
    static uint8_t nextFace = 0;
    if (name.length() == 0 || name == "next") {
        const FaceCommand& choice = faces[nextFace++ % (sizeof(faces) / sizeof(faces[0]))];
        brain.previewExpression(choice.expression);
        Serial.printf("[face] %s for 10 seconds\n", choice.name);
        return true;
    }
    for (const auto& choice : faces) {
        if (name == choice.name) {
            brain.previewExpression(choice.expression);
            Serial.printf("[face] %s for 10 seconds\n", choice.name);
            return true;
        }
    }
    Serial.println("[face] use: face [neutral|happy|sad|angry|disgruntled|surprised|sleepy|sleeping|suspicious|adoring|scared|vibe|vibe_active]");
    return false;
}

bool playAudioCommand(String requested) {
    requested.trim();
    requested.toLowerCase();
    if (requested.length() == 0 || requested == "list" || requested == "menu") {
        audio.printMenu();
        return true;
    }
    if (requested.startsWith("vol ") || requested.startsWith("volume ")) {
        const uint8_t offset = requested.startsWith("vol ") ? 4 : 7;
        String value = requested.substring(offset);
        value.trim();
        for (uint8_t i = 0; i < value.length(); ++i) {
            if (!isDigit(value[i])) {
                Serial.println("[audio] volume must be a number from 0 to 100.");
                return false;
            }
        }
        const int percent = value.toInt();
        if (percent < 0 || percent > 100) {
            Serial.println("[audio] volume must be a number from 0 to 100.");
            return false;
        }
        audio.setVolumePercent((uint8_t)percent);
        return true;
    }
    bool isNumber = requested.length() > 0;
    for (uint8_t i = 0; i < requested.length(); ++i) {
        if (!isDigit(requested[i])) { isNumber = false; break; }
    }
    if (isNumber) return audio.playNumber((uint8_t)requested.toInt());
    if (requested == "status") { audio.printStatus(); return true; }
    else if (requested == "tone") { audio.playTestTone(); return true; }
    else if (requested == "longtone") { audio.playLongTone(); return true; }
    else if (requested == "pintest") { inputs.suppressLoudNoise(20000); audio.runPinTest(); return true; }
    else if (requested == "mp3test") { audio.playTestMp3(); return true; }
    else if (requested == "cute") audio.playCute();
    else if (requested == "no") audio.playNo();
    else if (requested == "scared") audio.playScared();
    else if (requested == "shy") audio.playShy();
    else if (requested == "wakeup" || requested == "wake") audio.playWakeup();
    else if (requested == "yes") audio.playYes();
    else if (requested == "angry") audio.playVoxieAngry();
    else if (requested == "sleepy" || requested == "sleep") audio.playVoxieSleepy();
    else if (requested == "voxie_cute") audio.playVoxieCute();
    else if (requested == "voxie_angry") audio.playVoxieAngry();
    else if (requested == "voxie_sleepy") audio.playVoxieSleepy();
    else {
        Serial.println("[audio] type 'audio list', 'audio <1-20>', or 'audio vol <0-100>'.");
        return false;
    }
    Serial.printf("[audio] %s\n", requested.c_str());
    return true;
}

bool runServoCommand(String requested) {
    requested.trim();
    requested.toLowerCase();
    if (requested == "status") {
        Serial.printf("[servo] %s, PWM %s\n", servo.isMoving() ? "moving" : "resting",
                      servo.isAttached() ? "holding" : "released (quiet)");
        return true;
    }
    if (requested == "center") servo.center();
    else if (requested == "happy" || requested == "wag") servo.happyWag();
    else if (requested == "angry" || requested == "shake") servo.angryShake();
    else if (requested == "scared" || requested == "twitch") servo.startledTwitch();
    else if (requested == "wake" || requested == "nod") servo.wakeNod();
    else if (requested == "vibe" || requested == "left") servo.vibeStep(true);
    else if (requested == "right") servo.vibeStep(false);
    else if (requested == "test" || requested == "sweep") servo.diagnosticSweep();
    else {
        Serial.println("[servo] use: servo [status|test|center|happy|angry|scared|wake|vibe|left|right]");
        return false;
    }
    Serial.printf("[servo] %s\n", requested.c_str());
    return true;
}

// PetInputs mic tap: runs on the mic task (core 0) for every I2S block, so
// the stream keeps full rate however long loop() takes. The packet goes out
// in one Serial.write() call - the UART driver locks per call - so text
// printed from loop() can land between packets but never inside one.
void streamVoice(const int32_t* raw, size_t count) {
    static uint8_t packet[4 + 256 * sizeof(int16_t)];
    if (!voiceStream) return;
    count = min(count, (size_t)256);
    packet[0] = 0xA5;
    packet[1] = 0x5A;
    packet[2] = count & 0xFF;
    packet[3] = count >> 8;
    int16_t* pcm = (int16_t*)(packet + 4);
    // Same 24-bit-left-justified -> 16-bit reduction PetInputs uses.
    for (size_t i = 0; i < count; i++) pcm[i] = (int16_t)(raw[i] >> 16);
    Serial.write(packet, 4 + count * sizeof(int16_t));
}

void setVoiceStream(bool on) {
    if (on == voiceStream) return;
    if (!on) {
        voiceStream = false;
        delay(20);  // let an in-flight packet finish before the stream changes
    }
    Serial.println(on ? "[stream] start" : "[stream] stop");
    Serial.flush();
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT == 0
    Serial.updateBaudRate(on ? kStreamBaud : kNormalBaud);
#endif
    voiceStream = on;
}

// A touch's sound and body move follow how it felt (see PetMind's TouchFeeling).
void reactToTouch(bool wasAsleep) {
    const TouchFeeling feeling = brain.notifyTouched();
    if (feeling == TouchFeeling::ANNOYED) {
        servo.angryShake();
        audio.playNo();          // "enough already"
        return;
    }
    wasAsleep ? servo.wakeNod() : servo.happyWag();
    if (wasAsleep) {
        mind.energy() < 0.25f ? audio.playVoxieSleepy() : audio.playWakeup();
    } else if (feeling == TouchFeeling::MISSED_YOU) {
        audio.playWakeup();      // bright, excited "you're back!"
    } else if (feeling == TouchFeeling::LOVED) {
        audio.playShy();         // soft, content coo
    } else {
        audio.playCute();
    }
}

// Voice commands recognized on the PC (tools/mic_display.py).
void runVoiceCommand(String requested) {
    requested.trim();
    requested.toLowerCase();
    const bool wasAsleep = brain.isAsleep();
    if (requested == "voxie") {
        // Just the name: perk up and look attentive, waiting for a command.
        // A small chirp acknowledges it heard - unless it's dozing.
        brain.notifyWakeWord();
        servo.startledTwitch();
        if (!wasAsleep && mind.energy() >= 0.25f) audio.playYes();
    } else if (requested == "wake") {
        brain.notifyWakeWord();
        servo.wakeNod();
        // Woken while still worn out: a groggy sound instead of a bright one.
        mind.energy() < 0.25f ? audio.playVoxieSleepy() : audio.playWakeup();
    } else if (requested == "sleep") {
        brain.notifyGoToSleep();
        servo.center();
        audio.playVoxieSleepy();
    } else if (requested == "hey") {
        PetReply reply = mind.respond("hello");
        Serial.println(reply.spoken);
        brain.notifySpeak(reply);
        servo.happyWag();
        if (wasAsleep) audio.playWakeup();
        else if (mind.loneliness() > 0.3f) audio.playWakeup();   // so glad someone's here
        else mind.affectionLevel() > 0.6f ? audio.playShy() : audio.playCute();
    } else {
        Serial.println("[voice] use: voice [voxie|wake|sleep|hey]");
        return;
    }
    mind.recordVoice(requested);
    Serial.printf("[voice] %s (was %s)\n", requested.c_str(), wasAsleep ? "asleep" : "awake");
    mind.printMood();
}

void saveVolumeSetting() {
    if (!settingsPreferencesReady) {
        Serial.println("[settings] ERROR: volume preferences are unavailable; setting was not saved.");
        return;
    }
    if (settingsPreferences.putUChar("volume", audio.volumePercent()) != sizeof(uint8_t)) {
        Serial.println("[settings] ERROR: failed to save volume.");
        return;
    }
    Serial.printf("[settings] saved volume=%u%%\n", audio.volumePercent());
}

void adjustSettingsVolume(int direction) {
    const int currentLevel = (audio.volumePercent() + kVolumeStepPercent / 2) /
                             kVolumeStepPercent;
    const int adjusted = constrain((currentLevel + direction) * kVolumeStepPercent, 0, 100);
    settingsLastInputMs = millis();
    audio.setVolumePercent((uint8_t)adjusted);
    Serial.printf("[settings] volume=%u%% (right louder, left quieter)\n", audio.volumePercent());
    audio.playCute();
}

void updateSettingsButton(SettingsButton& button, uint8_t pin, uint32_t now) {
    const bool rawPressed = digitalRead(pin) == LOW;
    if (rawPressed != button.rawPressed) {
        button.rawPressed = rawPressed;
        button.rawChangedAtMs = now;
        Serial.printf("[settings] GPIO%u input=%s\n", pin, rawPressed ? "LOW/pressed" : "HIGH/released");
    }
    if (button.stablePressed == button.rawPressed ||
        (uint32_t)(now - button.rawChangedAtMs) < kButtonDebounceMs) {
        return;
    }

    button.stablePressed = button.rawPressed;
    if (button.stablePressed) {
        button.pressedAtMs = now;
        button.pressedInSettings = settingsMode;
        button.suppressRelease = false;
        if (settingsMode) settingsLastInputMs = now;
    } else {
        if (button.pressedInSettings && settingsMode && !button.suppressRelease) {
            adjustSettingsVolume(pin == RIGHT_BUTTON_PIN ? 1 : -1);
        }
        button.pressedInSettings = false;
        button.suppressRelease = false;
    }
}

void updateSettingsButtons() {
    const uint32_t now = millis();
    updateSettingsButton(leftSettingsButton, LEFT_BUTTON_PIN, now);
    updateSettingsButton(rightSettingsButton, RIGHT_BUTTON_PIN, now);

    if (!settingsMode && rightSettingsButton.stablePressed &&
        !rightSettingsButton.pressedInSettings && !leftSettingsButton.stablePressed &&
        (uint32_t)(now - rightSettingsButton.pressedAtMs) >= kSettingsHoldMs) {
        settingsMode = true;
        settingsLastInputMs = now;
        const uint8_t initialVolume = (uint8_t)min(
            100, ((audio.volumePercent() + kVolumeStepPercent / 2) / kVolumeStepPercent) *
                 kVolumeStepPercent);
        audio.setVolumePercent(initialVolume);
        rightSettingsButton.suppressRelease = true;
        Serial.printf("[settings] volume mode entered at %u%%; right= louder, left=quieter\n",
                      audio.volumePercent());
        audio.playYes();
    }

    if (settingsMode && leftSettingsButton.stablePressed && rightSettingsButton.stablePressed) {
        leftSettingsButton.suppressRelease = true;
        rightSettingsButton.suppressRelease = true;
        settingsMode = false;
        saveVolumeSetting();
        Serial.println("[settings] volume mode exited with both buttons.");
        audio.playYes();
    }

    if (settingsMode && (uint32_t)(now - settingsLastInputMs) >= kSettingsIdleMs) {
        settingsMode = false;
        saveVolumeSetting();
        Serial.println("[settings] volume mode exited after 30 seconds idle.");
        audio.playYes();
    }
}

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "stream on") { setVoiceStream(true); return; }
    if (line == "stream off") { setVoiceStream(false); return; }
    if (line.startsWith("voice ")) {
        runVoiceCommand(line.substring(6));
        return;
    }
    if (line == "t") {
        reactToTouch(brain.isAsleep());
        return;
    }
    if (line == "n") {
        const bool wasAsleep = brain.isAsleep();
        brain.notifyLoudNoise();
        wasAsleep ? servo.angryShake() : servo.startledTwitch();
        audio.playScared();
        return;
    }
    if (line == "T") { inputs.simulateToggleTip(); servo.angryShake(); return; }
    if (line == "m") {
        micDebug = !micDebug;
        Serial.printf("[mic] debug %s\n", micDebug ? "on" : "off");
        return;
    }
    if (line == "mood") { mind.printMood(); return; }
    if (line == "mood reset") { mind.resetMood(); mind.printMood(); return; }
    if (line == "face" || line.startsWith("face ")) {
        previewFace(line.length() > 4 ? line.substring(5) : "");
        return;
    }
    if (line == "audio" || line.startsWith("audio ")) {
        playAudioCommand(line.length() > 5 ? line.substring(6) : "");
        return;
    }
    if (line.startsWith("servo ")) {
        runServoCommand(line.substring(6));
        return;
    }
    if (line.length()) {
        PetReply reply = mind.respond(line);
        Serial.println(reply.spoken);
        brain.notifySpeak(reply);
        servo.happyWag();
        audio.playYes();
    }
}

void setup() {
    // Roomy TX buffer so the voice stream rarely blocks the mic task.
    Serial.setTxBufferSize(8192);
    Serial.begin(kNormalBaud);
    pinMode(LEFT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RIGHT_BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("[settings] buttons ready: left GPIO%u=%s, right GPIO%u=%s (press should read LOW)\n",
                  LEFT_BUTTON_PIN, digitalRead(LEFT_BUTTON_PIN) == LOW ? "LOW" : "HIGH",
                  RIGHT_BUTTON_PIN, digitalRead(RIGHT_BUTTON_PIN) == LOW ? "LOW" : "HIGH");
    oled.begin();
    oled.setDrawColor(1);
    face.setColors(1, 0);
    face.begin();
    mind.begin();
    brain.begin();
    inputs.setMicTap(streamVoice);
    inputs.begin();
    audio.begin();
    settingsPreferencesReady = settingsPreferences.begin("deskpet", false);
    if (settingsPreferencesReady) {
        audio.setVolumePercent(settingsPreferences.getUChar("volume", 100));
    } else {
        Serial.println("[settings] ERROR: could not open volume preferences; using default volume.");
    }
    // The mic hears the speaker: ignore loud-noise and beat detection for
    // the length of each sound plus its echo, so the pet never startles at
    // (or dances to) its own voice.
    audio.setPlayHook([](uint32_t durationMs) { inputs.suppressLoudNoise(durationMs + 400); });
    servo.begin();
    Serial.println("Voxie reactive mode: touch GPIO9, settings buttons GPIO16/17, mic WS7/SCK10/SD6, amp LRC12/BCLK13/DIN11, MG90S servo GPIO15 (quiet rest). Type t, n, face ..., audio ..., servo ..., voice ..., mood, or a message.");
}

void loop() {
    const bool petWasAsleep = brain.isAsleep();
    inputs.setAsleep(petWasAsleep);
    inputs.update();
    updateSettingsButtons();
    // Its voice follows its mood: lower and softer when tired, a touch
    // brighter when happy.
    audio.setMoodVoice(0.9f + 0.1f * mind.energy() + 0.1f * (mind.happiness() - 0.5f),
                       0.55f + 0.45f * mind.energy());
    if (inputs.touchEdge()) reactToTouch(brain.isAsleep());
    // Read touch before decoding another audio chunk, so a real touch always
    // gets priority instead of waiting behind an MP3 buffer refill.
    audio.update();
    const bool tipped = inputs.tippedOver();
    const bool justTipped = tipped && !wasTipped;
    const bool wasAsleep = brain.isAsleep();
    brain.notifyTipped(tipped);
    if (justTipped) {
        servo.angryShake();
        wasAsleep ? audio.playWakeup() : audio.playNo();
    }
    wasTipped = tipped;

    // Feed the beat detector before deciding that a peak is a scary sound.
    // A regular run of peaks becomes dance mode; irregular spikes still
    // trigger the normal startled reaction.
    const int32_t vibeMinimumPeak = brain.isAsleep()
        ? kAsleepVibeMinimumPeak : kVibeMinimumPeak;
    const bool loudMusicCandidate = !inputs.hearingSelf() &&
                                    inputs.lastPeak() >= vibeMinimumPeak &&
                                    inputs.noiseFloor() >= kVibeMinimumFloor;
    if (inputs.rawSampleCount() > 0) {
        if (loudMusicCandidate) {
            beat.update(inputs.lastPeak());
            lastLoudMusicCandidateMs = millis();
        } else if (millis() - lastLoudMusicCandidateMs > 1800) {
            beat.reset();
        }
    }
    brain.notifyMusicPlaying(beat.isMusicDetected());
    if (beat.isMusicDetected() != wasMusicPlaying) {
        Serial.printf("[music] %s\n", beat.isMusicDetected() ? "vibe mode on" : "vibe mode off");
        wasMusicPlaying = beat.isMusicDetected();
    }
    if (beat.beatEdge()) {
        brain.notifyMusicBeat();
        danceLeft = !danceLeft;
        face.lookAt(danceLeft ? -0.75f : 0.75f, -0.10f, 180);
        servo.vibeStep(danceLeft);
        Serial.printf("[music] beat %.0f BPM\n", beat.bpm());
    }

    if (inputs.loudNoiseEdge() && !beat.isMusicDetected() && !loudMusicCandidate) {
        const bool wasAsleep = brain.isAsleep();
        Serial.printf("[event] loud noise (peak=%ld)\n", (long)inputs.lastPeak());
        brain.notifyLoudNoise();
        wasAsleep ? servo.angryShake() : servo.startledTwitch();
        audio.playScared();
    }
    if (micDebug) {
        static uint32_t lastMicReport = 0;
        if (millis() - lastMicReport >= 500) {
            lastMicReport = millis();
            Serial.printf("[mic] peak=%ld floor=%.0f music=%s bpm=%.0f\n",
                          (long)inputs.lastPeak(), inputs.noiseFloor(),
                          beat.isMusicDetected() ? "yes" : "no", beat.bpm());
        }
    }
    handleSerial();
    brain.update();

    // Mood report for Serial and the PC voice window's mood bar: frequent
    // while that window is connected, occasional otherwise.
    static uint32_t lastMoodReportMs = 0;
    if (millis() - lastMoodReportMs >= (voiceStream ? 2000u : 15000u)) {
        lastMoodReportMs = millis();
        mind.printMood();
    }

    // Unprompted sounds, from mood: a lonely pet calls out for attention
    // every few minutes; a very happy, lively one chirps now and then. Never
    // while asleep or in the middle of another reaction.
    static uint32_t nextVocalizeMs = 60000;
    if (millis() >= nextVocalizeMs) {
        if (!brain.isAsleep() && !brain.isReacting() && !beat.isMusicDetected()) {
            if (mind.loneliness() > 0.5f) {
                Serial.println("[mood] calling out (lonely)");
                servo.startledTwitch();
                audio.playShy();
            } else if (mind.happiness() > 0.8f && mind.energy() > 0.5f && random(0, 3) == 0) {
                Serial.println("[mood] happy chirp");
                audio.playCute();
            }
        }
        nextVocalizeMs = millis() + (uint32_t)random(120000, 240000);
    }

    // Keep the face live while an MP3 plays. The old audio-only rendering
    // gate made touch reactions appear only after the sound had finished.
    // 30 FPS leaves ample time for the MP3 decoder between OLED transfers.
    const uint32_t now = millis();
    if (now - lastDisplayRefreshMs >= 33) {
        lastDisplayRefreshMs = now;
        oled.clearBuffer();
        if (settingsMode) {
            drawVolumeSettingsScreen(now);
        } else {
            face.update();
            if (beat.isMusicDetected()) drawVibeOverlay();
        }
        oled.sendBuffer();
    }

    // The same local behavior layer that drives autonomous eye glances also
    // gives the body a restrained, delayed response. Explicit reactions and
    // music moves take priority inside PetServo, and idle PWM is detached
    // after each motion so the MG90S does not buzz while resting.
    servo.followEyes(face.gazeX(), face.gazeY(), !brain.isAsleep());
    servo.update();
    delay(1);
}
