# desk-pet-eyes

A reactive desk pet built around procedural (not video-clip) eyes + mouth
animation for an ESP32-driven 1.5" SH1107 128x128 OLED. Everything is
drawn live every frame, so there are no image/GIF assets to store on flash,
and expressions blend smoothly into each other instead of cutting.

`src/main.cpp` is the full desk-pet firmware: it answers typed-in "voice"
commands (real weather + real time over WiFi, plus a hardcoded local
personality - see **PetMind** below), goes idle/asleep after a while, turns
disgruntled (or angry, if shaken) when tipped over, reacts to a **real loud
noise picked up by an I2S microphone** as angry-if-it-was-asleep or
scared-if-it-was-awake (with a matching sound out of the speaker - see
**PetVoice** below), acts cute when touched, and idly fidgets through small
random mood flickers that drift with how it's been treated - see **How the
pet decides what to show** below. The original bare expression-cycling demo
still exists at `examples/face_demo_main.cpp` for pure visual tuning (see the
bottom of this file for how to run it instead).

## Hardware assumed

- ESP32-S3 (this build targets `esp32-s3-devkitc-1` in `platformio.ini`;
  any ESP32-S3 Arduino-core board works, PSRAM not required at this canvas
  size)
- 1.5" SH1107 128x128 SPI OLED. The firmware configures this display in
  `src/main.cpp` using the U8g2 `SH1107_PIMORONI_128X128` driver.
- A wire or foil pad on GPIO9 for touch (the ESP32-S3's built-in capacitive
  touch input needs no sensor module).
- An I2S MEMS microphone (INMP441 or similar) for **real** loud-noise
  detection - see **PetInputs** below.
- An I2S amplifier (MAX98357A) + small speaker so the pet can react out
  loud - see **PetVoice** below.
- Tilt (an IMU like an MPU6050) isn't wired up yet - it's still simulated
  over Serial (see below).

## Wiring

Default pins in `src/main.cpp` (change to match your wiring):

**OLED (SH1107, four-wire software SPI)**

| OLED pin | ESP32-S3 pin |
|---|---|
| RES / RST | GPIO 3 |
| DC / RS | GPIO 4 |
| CS | GPIO 5 |
| SDA / DIN | GPIO 2 (SPI MOSI) |
| SCL / SCK | GPIO 1 (SPI clock) |
| BLK (backlight) | 3.3V (always on) |
| VCC | 3.3V |
| GND | GND |

Clock and data are explicitly set to GPIO 1 and GPIO 2 for software SPI.
GPIO 11 is reserved for amplifier DIN; do not wire the display to the
hardware SPI defaults.

**I2S microphone (INMP441)** - input bus, real loud-noise detection, runs on
`I2S_NUM_0`:

| Mic pin | ESP32-S3 pin |
|---|---|
| SD | GPIO 6 |
| SCK | GPIO 10 |
| WS | GPIO 7 |
| L/R | 3.3V (selects the right channel; verified with this Super Mini wiring) |
| VDD | 3.3V (5V will destroy the mic) |
| GND | GND |

**I2S amplifier (MAX98357A)** - output bus, kept on `I2S_NUM_1` so it never
contends with the mic for the I2S peripheral/sample rate:

| Amp pin | ESP32-S3 pin |
|---|---|
| LRC | GPIO 12 |
| BCLK | GPIO 13 |
| DIN | GPIO 11 |
| GAIN | not connected (defaults to 9dB) |
| SD | not connected (defaults to mixing L+R) |
| VIN / VCC | 5V (VIN/VBUS - needed for enough current to drive the speaker without browning out the board) |
| GND | GND |
| +, - | speaker |

**Touch pad** - GPIO 9 (any wire/foil pad, no module needed).

**Volume settings buttons** - two momentary buttons, with one side of each
button connected to GND and the other to its GPIO. The firmware uses internal
pull-ups, so no external resistors are needed:

| Button | ESP32-S3 pin |
|---|---|
| Left (quieter) | GPIO 16 |
| Right (louder / enter settings) | GPIO 17 |

**MG90S servo**

| Servo lead | Connection |
|---|---|
| Signal (yellow/orange) | GPIO 15 |
| Power (red) | Separate regulated 5V supply, at least 2A |
| Ground (brown/black) | Servo supply ground, tied to ESP32 GND |

The firmware uses ESP32Servo's dedicated hardware PWM at 50Hz, calibrated to
500–2400us, and commands a new trajectory point only once per 20ms frame. Do
not power the MG90S and the MAX98357A from a computer USB port. Put a 470–1000
uF electrolytic capacitor across the 5V and GND rails close to the servo/amp
(capacitor `+` to 5V, `-` to GND) to absorb their startup/current spikes.

All grounds (LCD, mic, amp) are common - tie them together on a shared rail
if you're breadboarding, or twist/joint them if soldering.

## Tweaking robot settings

The two buttons provide a simple volume adjustment mode:

1. Hold the **right** button for 3 seconds. Voxie plays a confirmation sound
   and enters volume mode.
2. Click **right** to raise the volume by 20%, or **left** to lower it by 20%.
   Voxie plays a short preview at the selected level after each click. Volume
   moves through five steps: 0%, 20%, 40%, 60%, 80%, and 100%. The OLED
   switches to an animated volume screen showing the level and button
   directions while settings mode is active.
3. Press both buttons together to save and exit immediately, or leave the
   buttons untouched for 30 seconds. On timeout, Voxie saves the selected
   volume to ESP32 nonvolatile storage and exits automatically. Changes are
   not saved if power is removed before either exit completes.

The saved master volume scales every synthesized robot sound; mood can still
make playback quieter when the pet is tired. The serial command `audio vol
<0-100>` can also change volume temporarily. When entering settings mode, an
existing volume is rounded to the nearest 20% step.

The current SH1107 OLED is monochrome, so eye color cannot be changed on this
display. To make eye color an adjustable setting, use a color display and
update the display driver and face-rendering colors in `src/main.cpp` and
`lib/RoboFace/`.

For code-level tuning, `src/main.cpp` contains the button pins, volume step,
entry hold, and idle timeout. `lib/VoxieAudio/VoxieAudio.cpp` contains the
synthesized sound patterns; `lib/RoboFace/RoboFace.cpp` contains face shapes
and expression rendering.

## Build

This is a PlatformIO project. Verified building and linking clean against
the real ESP32-S3 toolchain (`pio run`) before being handed off - flash and
confirm on your actual panel/mic/amp next, since none of the visual tuning
or noise-threshold defaults below have been checked on real hardware.

1. `cp include/Secrets.h.example include/Secrets.h` and fill in your WiFi
   SSID/password (`include/Secrets.h` is gitignored, so it never gets
   committed). Weather/time commands work over WiFi; everything else
   (touch, tip, noise, faces) works with no network at all.
2. Edit `WeatherConfig::kLatitude`/`kLongitude` in
   `lib/WeatherService/WeatherService.h` to your location, and
   `TimeConfig::kGmtOffsetSec` in `lib/TimeService/TimeService.h` to your
   timezone.
3. Build and flash:

```
pio run -t upload
pio device monitor
```

For the 4 MB ESP32-S3 Super Mini wiring above, build and flash the full
firmware with its matching partition layout:

```
pio run -e esp32-s3-super-mini -t upload
pio device monitor -e esp32-s3-super-mini
```

The Super Mini target reserves a LittleFS partition for the bundled clips and
uses the microphone on GPIO 6 to keep its data line separate from amplifier
DIN on GPIO 11.

For browser-based mic monitoring and labeled WAV recording from the ESP32
microphone, open `tools/voice_terminal.html` in Chrome or Edge, connect to the
board, and start the mic stream. It supports adaptive voice-activity capture,
manual room-noise recordings, WAV download/playback, and buttons for testing
the board's `voice voxie`, `wake`, `sleep`, and `hey` commands. It records
examples; speech recognition and learned wake-word matching are handled by
`tools/mic_display.py`, not in the browser page. Run `tools/mic_display.bat`
for live spoken commands from the ESP32 mic. It now listens immediately, so
you can say “Hey Voxie” without first completing voice calibration; the
optional “Calibrate voice” button remains available to learn your level.
The recognizer accepts direct phrases such as “sleep” and “wake up” by
default; “Require Voxie first” can be enabled when you want to avoid reacting
to background conversations. Common transcription variants such as “Foxie,”
“Boxy,” and “Nevaise” are treated as the wake name.

For an isolated MAX98357A output check on the alternate board (BCLK GPIO13,
LRC GPIO12, DIN GPIO11), build and upload `amplifier-test` instead:

```
pio run -e amplifier-test -t upload
```

It plays a short 440Hz tone at startup. The existing web control panel or a
serial terminal can play the synthesized sound buttons (`cute`, `yes`, `no`,
`scared`, `shy`, `wakeup`, `sleepy`, `angry`), `audio tone`,
`audio longtone`, `audio pintest`, `audio vol 0-100`, or `audio status`.
This test replaces the firmware on that board. The normal project firmware
uses an 8 MB partition map; do not flash it to a 4 MB board without first
using a compatible 4 MB partition layout.

Dependencies (`adafruit/Adafruit GFX Library`, `adafruit/Adafruit BusIO`,
`adafruit/Adafruit GC9A01A`, `bblanchon/ArduinoJson`) are declared in
`platformio.ini` - PlatformIO installs them automatically on first build.

## How the pet decides what to show

`PetBrain` (`lib/PetBrain/`) picks one expression per frame using a fixed
priority order, so simultaneous events resolve predictably instead of
fighting over the display:

1. **Tipped over** - disgruntled, escalating to angry if tipped several
   times within 4s (reads as being shaken)
2. **Loud noise** - angry if it was asleep when the noise happened (startled
   awake and annoyed about it), otherwise scared - either way, for ~2.2s
3. **Touched** - adoring (heart eyes), for ~2s
4. **Answering a command** - happy + talking mouth + an on-screen text
   reply, for a few seconds (duration scales with reply length)
5. **Idle too long** (30s of no input) - sleeping (closed-line eyes, "o"
   mouth, floating "Z Z Z")
6. Otherwise - idle-awake. Rather than sitting flat at neutral, it randomly
   fidgets every ~4-12s into a brief (1.3-1.8s) HAPPY, SUSPICIOUS, or SLEEPY
   flicker, weighted by `PetMind`'s affection/grump levels (see below) - a
   well-treated pet fidgets happy more often, a frequently shaken/scolded
   one fidgets wary more often. Most rolls just stay neutral, so it reads as
   occasional personality rather than a constant twitch.

`PetInputs` (`lib/PetInputs/`) is where sensor readings come from. Touch is
real (ESP32-S3 `touchRead()` on GPIO9). **Loud-noise detection is also real**,
via the I2S mic:

- `PetInputs::begin()` starts a continuous I2S RX stream on `I2S_NUM_0` at
  16kHz/32-bit (the INMP441 left-justifies 24-bit samples into that 32-bit
  frame).
- Every `update()`, `updateMic()` non-blockingly drains whatever the DMA
  buffer has queued (`i2s_read(..., ticks_to_wait=0)` - never stalls the
  render loop) and takes the peak absolute sample of that chunk.
- An adaptive ambient noise floor (`_noiseFloor`, a slow exponential moving
  average) tracks normal room level. A `loudNoiseEdge()` fires the moment a
  chunk's peak beats `max(kMinAbsThreshold, noiseFloor * kNoiseFactor)` - so
  it reacts to a sudden spike (clap, shout, door slam) relative to whatever
  "quiet" currently means in your room, not a single fixed volume. A 900ms
  cooldown after each trigger stops one loud event from firing repeatedly,
  and that same window is excluded from the floor average so the loud event
  itself doesn't drag the baseline up.
- While asleep, the noise detector uses the higher threshold
  `max(14000, noiseFloor * 3.5)` instead of the awake threshold
  `max(6000, noiseFloor * 2.0)`. Music/beat detection also requires a peak of
  at least 14000 while asleep (5000 while awake), so quieter ambient sound
  cannot wake the pet through the music path either. Touch and explicit voice
  commands can still wake it.
- Tuning knobs (`kNoiseFactor`, `kMinAbsThreshold`, `kNoiseCooldownMs`,
  `kAsleepNoiseFactor`, `kAsleepMinAbsThreshold`, and `kFloorAlpha`) live at
  the top of `PetInputs.cpp`; the asleep music threshold is in `main.cpp`.
  To tune for your actual mic/enclosure, flash, open `pio device monitor`,
  type `m` + Enter to turn on live level printing, and watch `peak` vs `floor`
  while you talk/clap at different distances - raise the applicable minimum
  threshold if quiet room noise still triggers it, lower it if claps don't.
- Tilt (orientation) still needs hardware this project doesn't have yet - an
  IMU like an MPU6050 - so it stays simulated via `simulateToggleTip()`,
  wired into the exact same `PetBrain::notifyTipped()` call a real IMU
  would use. Swapping it for a real sensor later only means editing
  `PetInputs.cpp`.

Since there's no speech-to-text hardware, "voice" commands are still typed
as a line of text over Serial - `PetMind::respond()` is the single entry
point a real STT pipeline (wake-word engine, phone app, cloud STT) would
feed into later, unchanged. The mic is used purely for ambient loudness
detection here, not speech recognition.

Over Serial (`pio device monitor`), you can:

- Type a line + Enter to talk to it - see **PetMind** below for what it
  understands
- Type `t` + Enter to simulate a touch (in addition to touching the real
  pad), `T` + Enter to toggle tipped-over (no IMU wired up yet), `N` +
  Enter to force a loud-noise reaction manually
- Type `m` + Enter to toggle live mic level printing, for threshold tuning
  (see above)

## PetVoice: reactive sound out of the speaker

`PetVoice` (`lib/PetVoice/`) drives the MAX98357A amp on its own I2S bus
(`I2S_NUM_1`, kept separate from the mic's `I2S_NUM_0` so playback and
listening never contend for the peripheral or sample rate). In keeping with
RoboFace's "draw everything live, store no assets" approach, sounds aren't
audio clips - they're short synthesized frequency-swept tones (a sine wave
with a fading start/end frequency and a fade in/out envelope so they don't
click), generated on the fly in `PetVoice::playSweep()`.

`main.cpp` fires one of these alongside the matching `PetBrain` notification,
right where the sensor edge is detected:

- Touch -> a short rising chirp (`playTouched()`), or a gentler wake-up blip
  (`playWake()`) if the touch is what woke the pet from sleep
- Newly tipped over -> a short low grumble (`playTipped()`), or the same
  wake-up blip if it was asleep
- A loud-noise edge -> a sharp rising yelp (`playStartled()`), regardless of
  sleep state (the face already differentiates angry-vs-scared for that -
  see `PetBrain::notifyLoudNoise()`)

Each call blocks for well under 200ms, which is fine since these only ever
fire on rare discrete events, not every frame.

## PetMind: a local, hardcoded "understanding" layer

**This is not a language model.** A real one - even a small one - needs far
more RAM/flash/compute than an ESP32 has (320KB RAM, no GPU, ~240MHz). What
`lib/PetMind/` actually does, entirely offline and entirely hardcoded in
`PetMind.cpp`:

- **Intent matching** - your typed line is checked against small hardcoded
  keyword/synonym lists (weather, time, date, greetings, "who are you",
  "how are you", praise, scolding, etc.) and the first list that matches
  wins. No scoring model, just `String::indexOf()` substring checks.
- **Varied replies** - each intent has 3 hardcoded response templates,
  picked at random, so it doesn't repeat itself verbatim every time.
- **Name memory** - saying "my name is ___", "call me ___", or "I'm ___"
  extracts the next word via plain string slicing and remembers it (ask
  "what's my name" later, or notice `{name}` show up in replies).
- **Affection/grump counters** - touching, tipping, praise ("good boy",
  "i love you"...), and scolding ("bad pet", "stupid"...) nudge two 0..1
  running values up or down. These are the numbers the idle-fidget system
  above reads to bias its personality - a pet you've praised and pet a lot
  will fidget happier; one you've shaken and scolded a lot will fidget
  warier.
- **Persisted to flash** - name, affection, grump, and interaction counts
  are saved to NVS (`Preferences`) so they survive a reboot. Writes are
  throttled to at most once per 30s to go easy on flash wear, so the very
  latest change or two can be lost if power cuts before the next save.
- **Stats** - ask "how many times" or "stats" and it reports its own
  touch/tip/command counts back to you.

All of this lives in `PetMind::respond()` and the keyword/template arrays
above it in `PetMind.cpp` - add a new intent by adding a keyword list, a
template list, and one more `if (containsAny(...))` branch.

### Weather/time/date get a clean data screen, not a sentence

Most replies (greetings, praise, name, stats, ...) show the face with a
short caption underneath, same as before. Weather/time/date are different:
they're a single glanceable fact, so the face steps aside entirely and the
screen shows just the value - "72F", "03:45 PM", "Tue Jul 09" - big and
centered, the way a watch face shows a complication instead of a paragraph.

To keep that transition from feeling like an abrupt cut, `PetBrain` closes
the eyes (a blink) first, waits ~220ms for it to visibly finish, *then*
switches to the data screen; when the answer's time is up it returns to
whatever the face would normally be doing (idle fidget, etc.), which eases
in via the same transition system every expression change already uses.
The full descriptive sentence ("It's 72 F and partly cloudy.") still gets
printed to Serial for logging - only the on-screen presentation is
minimal. See `PetBrain::notifySpeak()` / `isShowingDataScreen()` and
`main.cpp`'s `drawDataScreen()`.

## Using RoboFace in your own sketch

```cpp
GFXcanvas16 canvas(PANEL_W, PANEL_H);   // offscreen framebuffer
RoboFace face(&canvas, PANEL_W, PANEL_H);
face.setColors(0xFFFF, 0x0000);   // eye color, background color (RGB565)
face.begin();

face.setExpression(Expression::HAPPY, 0.35f); // transition time in seconds
face.setTalking(true);                        // idle mouth flutter
face.lookAt(0.6f, -0.3f);                      // manual gaze, -1..1 per axis
face.blinkOnce();

// in loop():
face.update();  // advances animation, draws into the canvas
tft.drawRGBBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H); // blit to panel
```

When idle (no manual `lookAt`/expression calls), RoboFace runs on its own:
it blinks every 2-6s and glances around every 1.2-3.5s so the face reads as
alive rather than static. Call `face.setAutonomous(false)` to disable that
and drive everything manually.

To drive the mouth from real audio (actual lip-sync instead of the built-in
flutter), call `face.setMouthOverride(true, amount)` each frame with your
audio envelope (0..1) instead of `setTalking(true)`.

## Tuning the look

Expression shapes (eye size/spacing/slant, mouth curve/width) are defined
in the `expressionParams()` table near the top of
`lib/RoboFace/RoboFace.cpp`, and the base pixel geometry they scale from
(`kBaseEyeW`, `kBaseEyeSpacing`, etc.) sits just below it. Nothing here has
been visually tuned on real hardware - flash it, then adjust those numbers
to taste for your specific 1.52" panel and desk-pet enclosure.

## Adding a new expression

1. Add a name to the `Expression` enum in `RoboFace.h`.
2. Add a matching case to `expressionParams()` in `RoboFace.cpp` with your
   own `FaceParams` values.
3. Call `face.setExpression(Expression::YourNewOne)`.

## Running the plain face demo instead

`examples/face_demo_main.cpp` is the earlier bare demo: it just cycles
every expression on a timer and takes single-letter Serial commands,
with no PetBrain, WiFi, or sensors involved - useful when you only want to
check how an expression looks. To run it, temporarily replace
`src/main.cpp` with its contents (or swap the two files) and build as
usual; swap back to get the full desk-pet behavior again.
