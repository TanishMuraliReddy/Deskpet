#include <Arduino.h>
#include <SPI.h>
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
#define OLED_CLK  12
#define OLED_DATA 11
#define OLED_CS   10
#define OLED_DC   9
#define OLED_RST  8
#define PANEL_W   128
#define PANEL_H   128

// INMP441 microphone and touch pad wiring.
#define MIC_SCK 16
#define MIC_WS  17
#define MIC_SD  15
#define TOUCH_PIN 1

// MAX98357A I2S speaker amplifier (separate I2S bus from the microphone).
#define AMP_LRC  4
#define AMP_BCLK 5
#define AMP_DIN  7
// Connect MAX98357A SD / SD_MODE here. LOW mutes the amp when idle; HIGH
// enables it only while a sound is being played.
#define AMP_SD   21

// Standard hobby-servo signal pin. GPIO18 is otherwise unused by the OLED,
// microphone, and amplifier. Servo power must come from a separate 5 V rail.
#define SERVO_PIN 18

// VIBE is intentionally reserved for clearly loud music, not speech or
// quiet household sounds. These values match the microphone's normalized
// peak scale printed by the `m` serial-debug command.
constexpr int32_t kVibeMinimumPeak = 5000;
constexpr float kVibeMinimumFloor = 1200.0f;

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

bool wasTipped = false;
bool wasMusicPlaying = false;
bool danceLeft = false;
bool micDebug = false;
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

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "t") {
        const bool wasAsleep = brain.isAsleep();
        brain.notifyTouched();
        wasAsleep ? servo.wakeNod() : servo.happyWag();
        wasAsleep ? audio.playWakeup() : audio.playCute();
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
    Serial.begin(230400);
    oled.begin();
    oled.setDrawColor(1);
    face.setColors(1, 0);
    face.begin();
    mind.begin();
    brain.begin();
    inputs.begin();
    audio.begin();
    servo.begin();
    Serial.println("Voxie reactive mode: touch GPIO1, mic GPIO15/16/17, amp BCLK5/LRC4/DIN7/SD21, MG90S servo GPIO18 (quiet rest). Type t, n, face ..., audio ..., servo ..., or a message.");
}

void loop() {
    inputs.update();
    if (inputs.touchEdge()) {
        const bool wasAsleep = brain.isAsleep();
        brain.notifyTouched();
        wasAsleep ? servo.wakeNod() : servo.happyWag();
        wasAsleep ? audio.playWakeup() : audio.playCute();
    }
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
    const bool loudMusicCandidate = inputs.lastPeak() >= kVibeMinimumPeak &&
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

    // Keep the face live while an MP3 plays. The old audio-only rendering
    // gate made touch reactions appear only after the sound had finished.
    // 30 FPS leaves ample time for the MP3 decoder between OLED transfers.
    const uint32_t now = millis();
    if (now - lastDisplayRefreshMs >= 33) {
        lastDisplayRefreshMs = now;
        oled.clearBuffer();
        face.update();
        if (beat.isMusicDetected()) drawVibeOverlay();
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
