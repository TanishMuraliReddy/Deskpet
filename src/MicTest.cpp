#if defined(MIC_TEST)
#include <Arduino.h>
#include <driver/i2s.h>

// Dedicated bring-up test for the INMP441 I2S microphone. Shares the
// production wiring (SCK=10, WS=7, SD=6) and the verified right I2S channel
// setup as PetInputs, but nothing else - no OLED, amp, servo, or touch spam.
//
// Two modes, switched over Serial:
//   (default)   meter  - prints a text level bar ~20 times a second
//   stream on   - streams 16 kHz PCM in the same
//                 framed packets as the main firmware (see streamVoice() in
//                 main.cpp), for tools/mic_terminal.py / mic_display.py
//   stream off  - back to 230400 and the text meter
constexpr int kMicSck = 10;
constexpr int kMicWs = 7;
constexpr int kMicSd = 6;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kMeterIntervalMs = 50;
constexpr uint32_t kNormalBaud = 230400;
constexpr uint32_t kStreamBaud = 921600;
constexpr size_t kReadSamples = 256;

int32_t rawBuf[kReadSamples];
int16_t pcmBuf[kReadSamples];
bool streaming = false;

int32_t windowPeak = 0;
uint32_t windowRawPeak = 0;
uint32_t windowShift8Peak = 0;
uint32_t windowShift14Peak = 0;
uint32_t windowSlot0Peak = 0;
uint32_t windowSlot1Peak = 0;
uint64_t windowSumSq = 0;
uint32_t windowCount = 0;
bool windowAllStuck = true;  // every raw word was 0 or -1: SD line dead/floating
uint32_t lastMeterMs = 0;

void setupMic() {
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = kSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = kMicSck,
        .ws_io_num = kMicWs,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = kMicSd,
    };
    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK ||
        i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        Serial.println("[mic-test] I2S init FAILED");
        while (true) delay(1000);
    }
}

void printMeter() {
    if (windowCount == 0) {
        Serial.println("[mic] no samples - I2S not clocking?");
        return;
    }
    const int32_t rms = (int32_t)sqrt((double)windowSumSq / windowCount);
    // Log-ish bar: speech near the mic is usually a few hundred to a few
    // thousand on this 16-bit scale, a clap can hit 20000+.
    const int bars = constrain((int)(log10f((float)max(rms, (int32_t)1)) * 10.0f) - 10, 0, 35);
    char bar[36];
    for (int i = 0; i < 35; i++) bar[i] = i < bars ? '#' : ' ';
    bar[35] = '\0';
    Serial.printf("[mic] pcm_peak=%6ld rms=%5ld raw=%08lx slot0=%lu slot1=%lu >>8=%lu >>14=%lu |%s|%s\n",
                  (long)windowPeak, (long)rms, (unsigned long)windowRawPeak,
                  (unsigned long)windowSlot0Peak, (unsigned long)windowSlot1Peak,
                  (unsigned long)windowShift8Peak, (unsigned long)windowShift14Peak, bar,
                  windowAllStuck ? "  <- all zero: check SD/L-R wiring" : "");
}

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    const bool on = line == "stream on";
    if ((!on && line != "stream off") || on == streaming) return;
    Serial.println(on ? "[stream] start" : "[stream] stop");
    Serial.flush();
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT == 0
    Serial.updateBaudRate(on ? kStreamBaud : kNormalBaud);
#endif
    streaming = on;
}

void setup() {
    Serial.begin(kNormalBaud);
    delay(250);
    setupMic();
    Serial.println("[mic-test] Mic on SCK10/WS7/SD6 (right channel). Talk or clap near it.");
}

void loop() {
    handleSerial();

    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
    const size_t samples = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < samples; i++) {
        if (rawBuf[i] != 0 && rawBuf[i] != -1) windowAllStuck = false;
        const uint32_t rawMagnitude = (uint32_t)llabs((int64_t)rawBuf[i]);
        const int32_t shift8 = rawBuf[i] >> 8;
        const int32_t shift14 = rawBuf[i] >> 14;
        windowRawPeak = max(windowRawPeak, rawMagnitude);
        if (i & 1) windowSlot1Peak = max(windowSlot1Peak, rawMagnitude);
        else windowSlot0Peak = max(windowSlot0Peak, rawMagnitude);
        windowShift8Peak = max(windowShift8Peak, (uint32_t)abs(shift8));
        windowShift14Peak = max(windowShift14Peak, (uint32_t)abs(shift14));
        // Same 24-bit-left-justified -> 16-bit reduction as PetInputs.
        const int32_t s = rawBuf[i] >> 16;
        pcmBuf[i] = (int16_t)s;
        const int32_t mag = s < 0 ? -s : s;
        if (mag > windowPeak) windowPeak = mag;
        windowSumSq += (uint64_t)((int64_t)s * s);
    }
    windowCount += samples;

    if (streaming) {
        const uint8_t header[4] = {0xA5, 0x5A, (uint8_t)(samples & 0xFF), (uint8_t)(samples >> 8)};
        Serial.write(header, sizeof(header));
        Serial.write((const uint8_t*)pcmBuf, samples * sizeof(int16_t));
    } else if (millis() - lastMeterMs >= kMeterIntervalMs) {
        lastMeterMs = millis();
        printMeter();
    } else {
        return;  // keep accumulating this meter window
    }
    {
        windowPeak = 0;
        windowRawPeak = 0;
        windowShift8Peak = 0;
        windowShift14Peak = 0;
        windowSlot0Peak = 0;
        windowSlot1Peak = 0;
        windowSumSq = 0;
        windowCount = 0;
        windowAllStuck = true;
    }
}
#endif
