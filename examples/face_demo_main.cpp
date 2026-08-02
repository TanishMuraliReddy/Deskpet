#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "RoboFace.h"

// ---- Wiring: adjust to match your ESP32 <-> GC9A01 connections ----
#define TFT_RST  8
#define TFT_DC   9
#define TFT_CS   10
// SCK/MOSI use the ESP32-S3 Arduino core's default hardware FSPI pins
// (SCK=12/MOSI=11) via Adafruit's hardware-SPI constructor below. Pass
// explicit sck/mosi pins to the constructor instead if your wiring differs.

// Most 1.52" round GC9A01 modules are 240x240. Check your panel's
// datasheet/listing and change this if yours differs.
#define PANEL_W 240
#define PANEL_H 240

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

// Off-screen canvas: RoboFace draws here every frame, then we blit it to the
// physical panel in one shot to avoid visible tearing/flicker.
GFXcanvas16 canvas(PANEL_W, PANEL_H);

RoboFace face(&canvas, PANEL_W, PANEL_H);

// Demo: cycle through expressions on a timer. Replace this with whatever
// triggers your desk pet's mood (sensor input, serial command, etc.).
struct DemoStep {
    Expression expr;
    uint32_t holdMs;
};
const DemoStep kDemo[] = {
    {Expression::NEUTRAL, 3000},
    {Expression::HAPPY, 2500},
    {Expression::SURPRISED, 1800},
    {Expression::SUSPICIOUS, 2200},
    {Expression::SAD, 2200},
    {Expression::DISGRUNTLED, 2000},
    {Expression::ANGRY, 2000},
    {Expression::SLEEPY, 2200},
    {Expression::SLEEPING, 3200},
    {Expression::ADORING, 2200},
    {Expression::SCARED, 1800},
};
const uint8_t kDemoLen = sizeof(kDemo) / sizeof(kDemo[0]);
uint8_t demoIndex = 0;
uint32_t demoStepAtMs = 0;

void setup() {
    Serial.begin(115200);

    tft.begin();
    tft.setRotation(0);

    face.setColors(/*eyeColor=*/0xFFFF /*white*/, /*bgColor=*/0x0000 /*black*/);
    face.begin();

    demoStepAtMs = millis() + kDemo[0].holdMs;
    Serial.println("RoboFace demo running. Send h/s/a/d/u/z/Z/p/l/c/n over Serial");
    Serial.println("for happy/sad/angry/disgruntled(d)/surprised/sleepy(z)/sleeping(Z)/suspicious(p)/adoring(l)/scared(c)/neutral,");
    Serial.println("or 'b' to blink, 't' to toggle talking.");
}

void loop() {
    uint32_t now = millis();

    if (now >= demoStepAtMs) {
        demoIndex = (demoIndex + 1) % kDemoLen;
        face.setExpression(kDemo[demoIndex].expr);
        demoStepAtMs = now + kDemo[demoIndex].holdMs;
    }

    if (Serial.available()) {
        switch (Serial.read()) {
            case 'h': face.setExpression(Expression::HAPPY); break;
            case 's': face.setExpression(Expression::SAD); break;
            case 'a': face.setExpression(Expression::ANGRY); break;
            case 'd': face.setExpression(Expression::DISGRUNTLED); break;
            case 'u': face.setExpression(Expression::SURPRISED); break;
            case 'z': face.setExpression(Expression::SLEEPY); break;
            case 'Z': face.setExpression(Expression::SLEEPING); break;
            case 'p': face.setExpression(Expression::SUSPICIOUS); break;
            case 'l': face.setExpression(Expression::ADORING); break;
            case 'c': face.setExpression(Expression::SCARED); break;
            case 'n': face.setExpression(Expression::NEUTRAL); break;
            case 'b': face.blinkOnce(); break;
            case 't': {
                static bool talking = false;
                talking = !talking;
                face.setTalking(talking);
                break;
            }
            default: break;
        }
    }

    face.update();
    tft.drawRGBBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H);
}
