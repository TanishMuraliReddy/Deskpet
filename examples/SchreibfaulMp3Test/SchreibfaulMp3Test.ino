/*
  Voxie MAX98357A MP3 quality test — ESP32-audioI2S edition

  This is intentionally a small, dedicated test.  It uses the same physical
  amplifier wiring already working on Voxie and reads the existing Cute1.mp3
  file from LittleFS (internal flash).  No SD card or changed GPIOs are used.

  MAX98357A: BCLK GPIO5, LRC GPIO4, DIN GPIO7
  Microphone: remains on I2S port 0 in the full firmware; this test reserves
  I2S port 1 to match that arrangement.
*/

#include <Arduino.h>
#include <LittleFS.h>
#include <Audio.h>
#include <driver/i2s.h>

constexpr int kAmpBclk = 5;
constexpr int kAmpLrc = 4;
constexpr int kAmpDin = 7;
constexpr char kTrack[] = "/Cute1.mp3";

constexpr const char* kTracks[] = {
  "/Cute1.mp3", "/Cute2.mp3", "/No1.mp3", "/No2.mp3", "/No3.mp3",
  "/Scared1.mp3", "/Scared2.mp3", "/Scared3.mp3", "/Scared4.mp3",
  "/Shy1.mp3", "/Shy2.mp3", "/Shy3.mp3", "/Shy4.mp3",
  "/VoxieAngry.mp3", "/VoxieCute.mp3", "/VoxieSleepy.mp3",
  "/Wakeup1.mp3", "/Wakeup2.mp3", "/Yes1.mp3", "/Yes2.mp3",
};
constexpr size_t kTrackCount = sizeof(kTracks) / sizeof(kTracks[0]);

// The v2 constructor is (internalDAC, channelEnabled, i2sPort).  The earlier
// `Audio(1)` mistakenly turned on the S3's unsupported internal DAC and
// crashed during static initialization.  Use the external MAX98357A on port 1.
Audio audio(false, 3, I2S_NUM_1);
bool ampI2sIdle = false;

void audio_info(const char *info) {
  Serial.print("[audio] ");
  Serial.println(info);
}

bool startTrack(const char* track) {
  // The MAX98357A is quietest with its I2S clocks stopped between clips.
  // Re-enable its already-configured I2S port just before another clip starts.
  if (ampI2sIdle) {
    i2s_start(I2S_NUM_1);
    i2s_zero_dma_buffer(I2S_NUM_1);
    ampI2sIdle = false;
    delay(2);
  }
  audio.stopSong();
  const bool started = audio.connecttoFS(LittleFS, track);
  Serial.printf("[audio] %s %s\n", started ? "playing" : "FAILED to open", track);
  return started;
}

void enterQuietIdle();

void printTrackList() {
  Serial.println("[audio] Local MP3 clips:");
  for (size_t i = 0; i < kTrackCount; ++i) {
    Serial.printf("  %u: %s\n", static_cast<unsigned>(i + 1), kTracks[i]);
  }
  Serial.println("[audio] Send a number (1-20), r for Cute1, +/- for volume, s to stop.");
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (!command.length()) return;
  if (command == "r" || command == "cute" || command == "cute1") {
    startTrack(kTrack);
  } else if (command == "l" || command == "list") {
    printTrackList();
  } else if (command == "s" || command == "stop") {
    audio.stopSong();
    enterQuietIdle();
    Serial.println("[audio] stopped");
  } else if (command == "+") {
    const uint8_t volume = min<uint8_t>(21, audio.getVolume() + 1);
    audio.setVolume(volume);
    Serial.printf("[audio] volume %u/21\n", volume);
  } else if (command == "-") {
    const uint8_t volume = audio.getVolume() ? audio.getVolume() - 1 : 0;
    audio.setVolume(volume);
    Serial.printf("[audio] volume %u/21\n", volume);
  } else {
    const int selection = command.toInt();
    if (selection >= 1 && selection <= static_cast<int>(kTrackCount) &&
        String(selection) == command) {
      startTrack(kTracks[selection - 1]);
    } else {
      Serial.println("[audio] use a number 1-20, list, r, +, -, or s.");
    }
  }
}

void enterQuietIdle() {
  if (ampI2sIdle) return;

  // Flush to digital silence first, then remove BCLK/LRC.  With no I2S
  // clocks, the MAX98357A cannot amplify idle bus noise or decoder residue.
  i2s_zero_dma_buffer(I2S_NUM_1);
  delay(3);
  i2s_stop(I2S_NUM_1);
  ampI2sIdle = true;
  Serial.println("[audio] quiet idle");
}

void setup() {
  // Match the ESP32-S3 ROM boot log, so opening the monitor never begins with
  // unreadable characters.  This has no effect on the I2S audio quality.
  Serial.begin(115200);
  delay(250);
  Serial.println("\nVoxie MP3 quality test (ESP32-audioI2S)");
  Serial.println("MAX pins unchanged: BCLK=5, LRC=4, DIN=7; storage=LittleFS.");

  if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    Serial.println("[audio] LittleFS mount failed. Upload the filesystem image, then retry.");
    return;
  }
  if (!LittleFS.exists(kTrack)) {
    Serial.println("[audio] /Cute1.mp3 is missing. Upload the filesystem image, then retry.");
    return;
  }

  audio.setPinout(kAmpBclk, kAmpLrc, kAmpDin);
  // The library default is only about 6 KB on a no-PSRAM configuration.  A
  // larger internal-RAM source buffer prevents flash-read timing jitter from
  // becoming audible dropouts on the MAX98357A.
  audio.setBufsize(64 * 1024, -1);
  audio.setVolume(15);       // Library range is 0 (silent) to 21 (maximum).
  // The MAX98357A's SD/MODE input is floating on Voxie, which already mixes
  // L/R internally. Preserve the source's stereo I2S frames instead of
  // performing a second software downmix.
  audio.forceMono(false);
  startTrack(kTrack);
  printTrackList();
}

void loop() {
  // This must run continuously; one FreeRTOS tick lets USB/serial and the
  // watchdog run without starving MP3/I2S buffer refills.
  audio.loop();
  vTaskDelay(1);

  // `stopSong()` runs at normal EOF.  Give the last DMA block time to drain,
  // then silence the I2S port instead of leaving a clocked idle amplifier.
  static uint32_t stoppedAt = 0;
  if (!audio.isRunning()) {
    if (stoppedAt == 0) stoppedAt = millis();
    if (millis() - stoppedAt >= 45) enterQuietIdle();
  } else {
    stoppedAt = 0;
  }

  static String commandBuffer;
  static uint32_t lastCommandCharacterMs = 0;
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') continue;
    if (character == '\n') {
      handleCommand(commandBuffer);
      commandBuffer = "";
    } else if (commandBuffer.length() < 12) {
      commandBuffer += character;
      lastCommandCharacterMs = millis();
    }
  }

  // The desktop monitor can send raw text without a newline.  Treat a short
  // quiet gap after the last byte as end-of-command, so `list`, `r`, or `12`
  // work with either monitor line-ending setting.
  if (commandBuffer.length() && millis() - lastCommandCharacterMs >= 35) {
    handleCommand(commandBuffer);
    commandBuffer = "";
  }
}
