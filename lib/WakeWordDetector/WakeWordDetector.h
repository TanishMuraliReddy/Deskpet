#pragma once
#include <Arduino.h>
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

// Wraps ESP-SR's standalone WakeNet engine directly (not the full AFE
// pipeline - that's for multi-mic arrays doing beamforming/echo
// cancellation, which this single INMP441 mic has no use for). Fed raw I2S
// mic samples from PetInputs::rawSamples() each loop (see its comment for
// why this doesn't do its own i2s_read()); internally rescales them to
// 16-bit PCM and buffers up to whatever chunk size the model wants before
// calling detect().
//
// The wake word is "Hi, ESP" (Espressif's wn9_hiesp model, enabled via
// sdkconfig.defaults), not literally "Voxie" - a real trained "Voxie" model
// needs a paid custom-training service (there's no free/DIY path to one);
// this is the free stand-in that ships today. See notifyMusicPlaying's
// sibling in PetBrain, notifyWakeWord(), for how it turns into a reaction.
class WakeWordDetector {
public:
    // Loads the model from the "model" partition. Returns false (and prints
    // why over Serial) if the model wasn't found - e.g. the partition
    // wasn't flashed, or the sdkconfig model choice doesn't match what's on
    // it. feed() becomes a harmless no-op in that case.
    bool begin();

    void feed(const int32_t* i2sSamples, size_t count);
    bool detected();  // true once, the first check after a wake word lands

private:
    const esp_wn_iface_t* _iface = nullptr;
    model_iface_data_t* _model = nullptr;
    int16_t* _chunkBuf = nullptr;
    int _chunkSize = 0;
    int _chunkFill = 0;
    bool _detected = false;
};
