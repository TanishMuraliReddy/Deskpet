#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "VoxieAudio.h"
#include "AudioAssets.h"
#include <driver/i2s.h>

using namespace VoxieAudioAssets;
namespace { mp3dec_t decoder; int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; int16_t stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; }

VoxieAudio::VoxieAudio(int bclkPin, int lrcPin, int dinPin) : _bclkPin(bclkPin), _lrcPin(lrcPin), _dinPin(dinPin) {}

void VoxieAudio::begin() {
    i2s_config_t config = { .mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX), .sample_rate=16000, .bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT, .channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT, .communication_format=I2S_COMM_FORMAT_STAND_I2S, .intr_alloc_flags=ESP_INTR_FLAG_LEVEL1, .dma_buf_count=4, .dma_buf_len=256, .use_apll=false, .tx_desc_auto_clear=true, .fixed_mclk=0 };
    i2s_pin_config_t pins = { .bck_io_num=_bclkPin, .ws_io_num=_lrcPin, .data_out_num=_dinPin, .data_in_num=I2S_PIN_NO_CHANGE };
    _ready = i2s_driver_install(I2S_NUM_1, &config, 0, nullptr) == ESP_OK && i2s_set_pin(I2S_NUM_1, &pins) == ESP_OK;
    if (!_ready) Serial.println("VoxieAudio: I2S amp init failed.");
}

void VoxieAudio::play(size_t offset, size_t size) {
    if (!_ready) return;
    mp3dec_init(&decoder);
    _offset = offset; _end = offset + size; _playing = true;
}

void VoxieAudio::update() {
    if (!_playing) return;
    if (_offset >= _end) { _playing = false; return; }
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&decoder, VoxieAudioData + _offset, _end - _offset, pcm, &info);
    if (info.frame_bytes <= 0) { _offset++; return; }
    _offset += info.frame_bytes;
    if (samples <= 0) return;
    i2s_set_clk(I2S_NUM_1, info.hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    const int16_t* output = pcm;
    size_t count = samples * info.channels;
    if (info.channels == 1) { for (int i=0;i<samples;i++) { stereo[i*2]=pcm[i]; stereo[i*2+1]=pcm[i]; } output=stereo; count=samples*2; }
    size_t written = 0;
    i2s_write(I2S_NUM_1, output, count * sizeof(int16_t), &written, 0);
}

void VoxieAudio::playCute(){ Clip c=random(2)?Cute1:Cute2; play(c.offset,c.size); }
void VoxieAudio::playNo(){ Clip a[]={No1,No2,No3}; Clip c=a[random(3)]; play(c.offset,c.size); }
void VoxieAudio::playScared(){ Clip a[]={Scared1,Scared2,Scared3,Scared4}; Clip c=a[random(4)]; play(c.offset,c.size); }
void VoxieAudio::playShy(){ Clip a[]={Shy1,Shy2,Shy3,Shy4}; Clip c=a[random(4)]; play(c.offset,c.size); }
void VoxieAudio::playWakeup(){ Clip c=random(2)?Wakeup1:Wakeup2; play(c.offset,c.size); }
void VoxieAudio::playYes(){ Clip c=random(2)?Yes1:Yes2; play(c.offset,c.size); }
