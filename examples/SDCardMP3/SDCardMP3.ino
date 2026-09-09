// Standalone ESP32-S3 SD-card MP3 player for Voxie's MAX98357A.
// Library dependencies:
//   arduino-audio-tools by pschatzmann (v1.2.2)
//   arduino-libhelix by pschatzmann (v0.9.2)

// Enables AudioSourceSD's constructor that accepts our dedicated SPI bus.
#define USE_SD_SUPPORTS_SPI

#include <SPI.h>
#include <SD.h>
#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>
#include <AudioTools/Disk/AudioSourceSD.h>

using namespace audio_tools;

// Keep Voxie's existing, known-working MAX98357A wiring.
constexpr int AMP_BCLK = 5;
constexpr int AMP_LRC  = 4;
constexpr int AMP_DIN  = 7;

// Dedicated SD-card SPI bus. These are free pins on the ESP32-S3 DevKitC-1
// N8R2 and do not collide with the LCD, microphone, touch pad, servo, or amp.
constexpr int SD_CS   = 42;
constexpr int SD_MOSI = 40;
constexpr int SD_MISO = 39;
constexpr int SD_SCK  = 41;

SPIClass sdSpi(FSPI);
AudioSourceSD source("/", ".mp3", SD_CS, sdSpi);
I2SStream i2s;
MP3DecoderHelix decoder;

// AudioPlayer owns the StreamCopy/copy pipeline:
// SD file -> Helix MP3 decoder -> I2S stream -> MAX98357A.
AudioPlayer player(source, i2s, decoder);
bool isPlaying = false;

void stopWithError(const char* message) {
  Serial.println(message);
  while (true) delay(1000);
}

void printHelp() {
  Serial.println("Commands: help | play | stop | status | vol <0-100>");
}

void playMusic() {
  player.stop();
  isPlaying = player.playPath("/music.mp3");
  if (isPlaying) {
    Serial.println("[audio] playing /music.mp3");
  } else {
    Serial.println("[audio] cannot open /music.mp3");
  }
}

void handleCommand() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();
  if (command == "help" || command == "?") {
    printHelp();
  } else if (command == "play" || command == "restart") {
    playMusic();
  } else if (command == "stop") {
    player.stop();
    isPlaying = false;
    Serial.println("[audio] stopped");
  } else if (command == "status") {
    Serial.printf("[sd] card=%s  [audio] playing=%s  file=/music.mp3\n",
                  SD.cardType() == CARD_NONE ? "not mounted" : "mounted",
                  isPlaying ? "yes" : "no");
  } else if (command.startsWith("vol ")) {
    const int percent = constrain(command.substring(4).toInt(), 0, 100);
    player.setVolume(percent / 100.0f);
    Serial.printf("[audio] volume=%d%%\n", percent);
  } else if (command.length() != 0) {
    Serial.println("Unknown command. Type help.");
  }
}

void setup() {
  Serial.begin(230400);
  delay(300);
  Serial.println("SD MP3 player starting");

  // Starts the user-assigned SPI pins. source.begin() below mounts the card.
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  // Bring a breadboarded card up at a deliberately conservative clock. Once
  // it is mounted, AudioSourceSD reuses this already-initialized SD instance.
  if (!SD.begin(SD_CS, sdSpi, 400000)) {
    stopWithError("SD electrical handshake failed. Check CS=42, MOSI=40, MISO=39, SCK=41, 3.3V, and GND.");
  }
  if (!source.begin()) {
    stopWithError("SD card mount failed. Check 3.3V, GND, and CS/MOSI/MISO/SCK wiring.");
  }

  auto config = i2s.defaultConfig(TX_MODE);
  config.port_no = 1;               // I2S1; I2S0 remains free for the microphone.
  config.pin_bck = AMP_BCLK;
  config.pin_ws = AMP_LRC;
  config.pin_data = AMP_DIN;
  config.sample_rate = 44100;
  config.channels = 2;
  config.bits_per_sample = 16;
  config.i2s_format = I2S_STD_FORMAT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.buffer_count = 8;
  config.buffer_size = 256;
  if (!i2s.begin(config)) {
    stopWithError("MAX98357A I2S setup failed.");
  }

  player.setAutoNext(false);
  player.setVolume(0.9f);
  if (!player.begin(0, false)) {
    stopWithError("Audio player could not initialize the SD source.");
  }
  Serial.printf("[sd] mounted, %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  printHelp();
  playMusic();
}

void loop() {
  // Non-blocking StreamCopy owned by AudioPlayer. Call it constantly.
  handleCommand();
  if (isPlaying) {
    player.copy();
    if (!player.isActive()) {
      isPlaying = false;
      Serial.println("[audio] finished");
    }
  }
}
