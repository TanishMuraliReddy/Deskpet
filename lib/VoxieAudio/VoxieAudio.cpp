#include "VoxieAudio.h"

#include <AudioTools.h>
#include <math.h>

using namespace audio_tools;

namespace {
constexpr uint32_t kSampleRate = 24000;
constexpr uint32_t kFramesPerChunk = 144;
constexpr float kPi2 = 6.28318530718f;

// Each segment is an original little electronic vocalisation.  Frequency
// glides, a gentle harmonic, and a soft envelope make the sounds expressive
// without relying on compressed audio files or imitating another robot.
struct ToneSegment {
    uint16_t durationMs;
    float startHz;
    float endHz;
    float level;
    float harmonic;
    float wobbleHz;
};

constexpr ToneSegment kCute[] = {
    {100, 720, 980, 0.30f, 0.14f, 4.0f},
    {55,     0,   0, 0.00f, 0.00f, 0.0f},
    {150, 1080, 1450, 0.34f, 0.12f, 5.0f},
};
constexpr ToneSegment kYes[] = {
    {105, 680, 920, 0.30f, 0.12f, 3.0f},
    {125, 1040, 1280, 0.32f, 0.10f, 3.0f},
};
constexpr ToneSegment kNo[] = {
    {160, 690, 500, 0.30f, 0.17f, 2.0f},
    {145, 570, 390, 0.27f, 0.18f, 2.0f},
};
constexpr ToneSegment kScared[] = {
    {75,  860, 1220, 0.31f, 0.11f, 9.0f},
    {70, 1200, 1620, 0.34f, 0.09f, 11.0f},
    {95, 1500, 1050, 0.27f, 0.10f, 8.0f},
};
constexpr ToneSegment kShy[] = {
    {95,  730, 870, 0.20f, 0.08f, 5.0f},
    {65,    0,   0, 0.00f, 0.00f, 0.0f},
    {120, 820, 690, 0.18f, 0.10f, 4.0f},
};
constexpr ToneSegment kWakeup[] = {
    {115, 460, 720, 0.26f, 0.10f, 3.0f},
    {130, 760, 1060, 0.30f, 0.11f, 4.0f},
    {105, 1110, 1260, 0.25f, 0.08f, 3.0f},
};
constexpr ToneSegment kSleepy[] = {
    {220, 510, 410, 0.16f, 0.07f, 2.0f},
    {95,    0,   0, 0.00f, 0.00f, 0.0f},
    {260, 430, 330, 0.13f, 0.06f, 1.5f},
};
constexpr ToneSegment kAngry[] = {
    {130, 430, 330, 0.34f, 0.28f, 5.0f},
    {55,    0,   0, 0.00f, 0.00f, 0.0f},
    {180, 390, 255, 0.36f, 0.30f, 4.0f},
};
constexpr ToneSegment kTestTone[] = {
    {700, 440, 440, 0.24f, 0.00f, 0.0f},
};

I2SStream robotI2s;

void patternFor(VoxieAudio::RobotSound sound, const ToneSegment*& pattern,
                size_t& count, const char*& label) {
    switch (sound) {
        case VoxieAudio::RobotSound::CUTE:      pattern = kCute;     count = sizeof(kCute) / sizeof(kCute[0]); label = "cute"; break;
        case VoxieAudio::RobotSound::YES:       pattern = kYes;      count = sizeof(kYes) / sizeof(kYes[0]); label = "yes"; break;
        case VoxieAudio::RobotSound::NO:        pattern = kNo;       count = sizeof(kNo) / sizeof(kNo[0]); label = "no"; break;
        case VoxieAudio::RobotSound::SCARED:    pattern = kScared;   count = sizeof(kScared) / sizeof(kScared[0]); label = "scared"; break;
        case VoxieAudio::RobotSound::SHY:       pattern = kShy;     count = sizeof(kShy) / sizeof(kShy[0]); label = "shy"; break;
        case VoxieAudio::RobotSound::WAKEUP:    pattern = kWakeup;  count = sizeof(kWakeup) / sizeof(kWakeup[0]); label = "wakeup"; break;
        case VoxieAudio::RobotSound::SLEEPY:    pattern = kSleepy;  count = sizeof(kSleepy) / sizeof(kSleepy[0]); label = "sleepy"; break;
        case VoxieAudio::RobotSound::ANGRY:     pattern = kAngry;   count = sizeof(kAngry) / sizeof(kAngry[0]); label = "angry"; break;
        case VoxieAudio::RobotSound::TEST_TONE: pattern = kTestTone; count = sizeof(kTestTone) / sizeof(kTestTone[0]); label = "tone"; break;
    }
}
}  // namespace

VoxieAudio::VoxieAudio(int bclkPin, int lrcPin, int dinPin, int ampEnablePin)
    : _bclkPin(bclkPin), _lrcPin(lrcPin), _dinPin(dinPin),
      _ampEnablePin(ampEnablePin) {}

void VoxieAudio::begin() {
    if (_ampEnablePin >= 0) {
        pinMode(_ampEnablePin, OUTPUT);
        digitalWrite(_ampEnablePin, LOW);
    }

    auto cfg = robotI2s.defaultConfig(TX_MODE);
    cfg.port_no = I2S_NUM_1;
    cfg.pin_bck = _bclkPin;
    cfg.pin_ws = _lrcPin;
    cfg.pin_data = _dinPin;
    cfg.sample_rate = kSampleRate;
    cfg.channels = 2;
    cfg.bits_per_sample = 16;
    cfg.i2s_format = I2S_STD_FORMAT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.buffer_count = 8;
    cfg.buffer_size = 256;
    if (!robotI2s.begin(cfg)) {
        _initError = "I2S1 output setup failed";
        Serial.println("[audio] I2S1 robot-sound setup FAILED.");
        return;
    }

    _ready = true;
    _initError = nullptr;
    Serial.printf("[audio] original robot-sound engine ready: I2S1 BCLK=%d LRC=%d DIN=%d. No MP3 decoder.\n",
                  _bclkPin, _lrcPin, _dinPin);
}

void VoxieAudio::update() {
    // Robot sounds are short and rendered directly when requested.
}

bool VoxieAudio::startOutput() {
    if (!robotI2s.isActive()) {
        Serial.println("[audio] I2S output is not active.");
        return false;
    }
    setAmpEnabled(true);
    delay(2);
    _outputRunning = true;
    return true;
}

void VoxieAudio::silenceAndStopOutput() {
    // I2S remains configured. If SD_MODE is wired to GPIO21, this also
    // removes idle hiss by placing the MAX98357A in hardware shutdown.
    setAmpEnabled(false);
    _outputRunning = false;
}

void VoxieAudio::setAmpEnabled(bool enabled) {
    if (_ampEnablePin >= 0) digitalWrite(_ampEnablePin, enabled ? HIGH : LOW);
}

void VoxieAudio::playRobotSound(RobotSound sound) {
    if (!_ready) {
        Serial.printf("[audio] robot sound unavailable: %s.\n",
                      _initError ? _initError : "not ready");
        return;
    }
    if (!startOutput()) return;

    const ToneSegment* pattern = nullptr;
    size_t count = 0;
    const char* label = "unknown";
    patternFor(sound, pattern, count, label);
    _playing = true;
    const float userLevel = _volumePercent / 100.0f;
    int16_t frames[kFramesPerChunk * 2];

    for (size_t segmentIndex = 0; segmentIndex < count; ++segmentIndex) {
        const ToneSegment& segment = pattern[segmentIndex];
        const uint32_t totalFrames = max(1UL, (uint32_t)segment.durationMs * kSampleRate / 1000UL);
        float phase = 0.0f;
        float wobblePhase = 0.0f;
        for (uint32_t base = 0; base < totalFrames; base += kFramesPerChunk) {
            const uint32_t frameCount = min(kFramesPerChunk, totalFrames - base);
            for (uint32_t i = 0; i < frameCount; ++i) {
                const float progress = (float)(base + i) / (float)totalFrames;
                const float envelope = min(1.0f, min(progress * 18.0f, (1.0f - progress) * 18.0f));
                const float frequency = segment.startHz + (segment.endHz - segment.startHz) * progress;
                phase += kPi2 * frequency / kSampleRate;
                if (phase >= kPi2) phase -= kPi2;
                wobblePhase += kPi2 * segment.wobbleHz / kSampleRate;
                if (wobblePhase >= kPi2) wobblePhase -= kPi2;
                const float voice = sinf(phase) + segment.harmonic * sinf(phase * 2.0f);
                const float tremolo = 0.92f + 0.08f * sinf(wobblePhase);
                const int16_t sample = (int16_t)(voice * tremolo * envelope *
                                                 segment.level * userLevel * 32760.0f);
                frames[i * 2] = sample;
                frames[i * 2 + 1] = sample;
            }
            robotI2s.write(reinterpret_cast<const uint8_t*>(frames), frameCount * sizeof(int16_t) * 2);
        }
    }

    delay(20);  // allow final DMA frames to leave the amplifier
    _playing = false;
    silenceAndStopOutput();
    Serial.printf("[audio] robot sound: %s\n", label);
}

void VoxieAudio::printStatus() const {
    Serial.printf("[audio] mode=original robot sounds ready=%s playing=%s volume=%u%% SD_GPIO=%d error=%s\n",
                  _ready ? "yes" : "no", _playing ? "yes" : "no", _volumePercent,
                  _ampEnablePin, _initError ? _initError : "none");
}

void VoxieAudio::printMenu() const {
    Serial.println("\n========== VOXIE ROBOT SOUNDS ==========");
    Serial.println(" 1 cute     2 yes      3 no       4 scared");
    Serial.println(" 5 shy      6 wakeup   7 sleepy   8 angry");
    Serial.println("Play:   audio <number> or audio [cute|yes|no|scared|shy|wakeup|sleepy|angry]");
    Serial.println("Volume: audio vol <0-100>   e.g. audio vol 85");
}

bool VoxieAudio::playNumber(uint8_t number) {
    switch (number) {
        case 1: playCute(); break;
        case 2: playYes(); break;
        case 3: playNo(); break;
        case 4: playScared(); break;
        case 5: playShy(); break;
        case 6: playWakeup(); break;
        case 7: playVoxieSleepy(); break;
        case 8: playVoxieAngry(); break;
        default:
            Serial.println("[audio] choose a robot-sound number from 1 to 8.");
            return false;
    }
    return true;
}

void VoxieAudio::setVolumePercent(uint8_t percent) {
    _volumePercent = min(percent, (uint8_t)100);
    Serial.printf("[audio] robot-sound volume=%u%%\n", _volumePercent);
}

void VoxieAudio::playTestTone() { playRobotSound(RobotSound::TEST_TONE); }

void VoxieAudio::playTestMp3() {
    Serial.println("[audio] MP3 test is disabled; use audio cute, angry, sleepy, or audio list.");
}

void VoxieAudio::playCute()          { playRobotSound(RobotSound::CUTE); }
void VoxieAudio::playNo()            { playRobotSound(RobotSound::NO); }
void VoxieAudio::playScared()        { playRobotSound(RobotSound::SCARED); }
void VoxieAudio::playShy()           { playRobotSound(RobotSound::SHY); }
void VoxieAudio::playWakeup()        { playRobotSound(RobotSound::WAKEUP); }
void VoxieAudio::playYes()           { playRobotSound(RobotSound::YES); }
void VoxieAudio::playVoxieCute()     { playRobotSound(RobotSound::CUTE); }
void VoxieAudio::playVoxieAngry()    { playRobotSound(RobotSound::ANGRY); }
void VoxieAudio::playVoxieSleepy()   { playRobotSound(RobotSound::SLEEPY); }
