#include <Arduino.h>
#include <U8g2lib.h>

// Dedicated bring-up test for the 1.5 in 128x128 SH1107, seven-pin SPI OLED.
// It deliberately shares the production wiring, but has no Wi-Fi, audio, or
// servo dependencies that could obscure a display wiring problem.
constexpr uint8_t kOledClk = 12;
constexpr uint8_t kOledData = 11;
constexpr uint8_t kOledCs = 10;
constexpr uint8_t kOledDc = 9;
constexpr uint8_t kOledReset = 8;

U8G2_SH1107_PIMORONI_128X128_F_4W_SW_SPI oled(
    U8G2_R0, kOledClk, kOledData, kOledCs, kOledDc, kOledReset);

void setup() {
    Serial.begin(230400);
    delay(250);
    Serial.println("[oled-test] Initializing SH1107 128x128 4-wire SPI display");
    oled.begin();
    oled.setPowerSave(0);
}

void loop() {
    static uint8_t phase = 0;
    oled.clearBuffer();

    // A large high-contrast pattern makes a successful electrical connection
    // obvious even if the font or orientation is not what was expected.
    for (uint8_t y = 0; y < 128; y += 16) {
        for (uint8_t x = 0; x < 128; x += 16) {
            if (((x / 16) + (y / 16) + phase) & 1) oled.drawBox(x, y, 16, 16);
        }
    }
    oled.setDrawColor(2);
    oled.drawFrame(2, 2, 124, 124);
    oled.drawBox(8, 48, 112, 33);
    oled.setDrawColor(0);
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(25, 61, "SH1107 SPI");
    oled.drawStr(31, 77, "GPIO 12/11");
    oled.setDrawColor(1);
    oled.sendBuffer();

    phase ^= 1;
    delay(700);
}
