#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_GFX.h>
#include "RoboFace.h"
#include "PetBrain.h"
#include "PetInputs.h"
#include "PetMind.h"

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

bool wasTipped = false;

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "t") { brain.notifyTouched(); return; }
    if (line == "n") { brain.notifyLoudNoise(); return; }
    if (line == "T") { inputs.simulateToggleTip(); return; }
    if (line.length()) {
        PetReply reply = mind.respond(line);
        Serial.println(reply.spoken);
        brain.notifySpeak(reply);
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
    Serial.println("Voxie OLED reactive mode: touch GPIO1, mic GPIO15/16/17. Type t, n, or a message.");
}

void loop() {
    inputs.update();
    if (inputs.touchEdge()) brain.notifyTouched();
    const bool tipped = inputs.tippedOver();
    brain.notifyTipped(tipped);
    wasTipped = tipped;
    if (inputs.loudNoiseEdge()) {
        Serial.printf("[event] loud noise (peak=%ld)\n", (long)inputs.lastPeak());
        brain.notifyLoudNoise();
    }
    handleSerial();
    brain.update();

    oled.clearBuffer();
    face.update();
    oled.sendBuffer();
    delay(20); // stable 50 FPS with autonomous blink and gaze
}
