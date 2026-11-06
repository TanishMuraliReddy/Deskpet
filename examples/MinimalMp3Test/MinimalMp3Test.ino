// Minimal MAX98357A MP3 test for Voxie's ESP32-S3.
// This deliberately excludes the OLED, microphone, servo, touch input, and
// PetBrain so the AudioTools pipeline can be tested in isolation.

#include <Arduino.h>
#include <LittleFS.h>
#include <AudioTools.h>
#include <AudioTools/Disk/AudioSourceLittleFS.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>

using namespace audio_tools;

constexpr int MAX_BCLK = 13;
constexpr int MAX_LRC = 12;
constexpr int MAX_DIN = 14;
constexpr uint32_t SAMPLE_RATE = 44100;

AudioSourceLittleFS source("/", "mp3");
I2SStream i2s;
MP3DecoderHelix decoder;
AudioPlayer player(source, i2s, decoder);

bool playing = false;

void printMetaData(MetaDataType type, const char* value, int) {
  Serial.printf("[mp3] %s: %s\n", toStr(type), value);
}

void startCute1() {
  player.stop();
  playing = player.playPath("/Cute1.mp3");
  if (playing) {
    Serial.println("[mp3] playing /Cute1.mp3");
  } else {
    Serial.println("[mp3] FAILED to open /Cute1.mp3");
  }
}

void setup() {
  Serial.begin(230400);
  delay(300);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    Serial.println("[mp3] LittleFS mount FAILED");
    return;
  }

  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.port_no = 1;
  cfg.pin_bck = MAX_BCLK;
  cfg.pin_ws = MAX_LRC;
  cfg.pin_data = MAX_DIN;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  cfg.i2s_format = I2S_STD_FORMAT;
  cfg.buffer_count = 8;
  cfg.buffer_size = 256;
  if (!i2s.begin(cfg)) {
    Serial.println("[mp3] I2S setup FAILED");
    return;
  }

  player.setAutoNext(false);
  player.setVolume(0.40f);  // reference-library volume, below clipping
  player.setMetadataCallback(printMetaData);
  if (!player.begin(0, false)) {
    Serial.println("[mp3] player setup FAILED");
    return;
  }
  Serial.println("[mp3] minimal test ready. Send r to replay Cute1.mp3.");
  startCute1();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') startCute1();
  if (playing) {
    player.copy();
    if (!player.isActive()) {
      playing = false;
      Serial.println("[mp3] finished");
    }
  }
}
