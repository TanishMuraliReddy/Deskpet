#include "WakeWordDetector.h"

bool WakeWordDetector::begin() {
    srmodel_list_t* models = esp_srmodel_init("model");
    if (!models || models->num <= 0) {
        Serial.println("WakeWordDetector: no models found on the 'model' partition.");
        return false;
    }

    char* wnName = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
    if (!wnName) {
        Serial.println("WakeWordDetector: 'hilexin' wake word model not found among packed models.");
        return false;
    }

    _iface = esp_wn_handle_from_name(wnName);
    _model = _iface->create(wnName, DET_MODE_90);
    _chunkSize = _iface->get_samp_chunksize(_model);
    _chunkBuf = (int16_t*)malloc(_chunkSize * sizeof(int16_t));
    _chunkFill = 0;

    Serial.printf("WakeWordDetector: loaded %s (\"%s\"), chunk=%d samples, rate=%dHz\n",
                  wnName, esp_wn_wakeword_from_name(wnName), _chunkSize,
                  _iface->get_samp_rate(_model));
    return true;
}

void WakeWordDetector::feed(const int32_t* i2sSamples, size_t count) {
    if (!_iface || !_model) return;

    for (size_t i = 0; i < count; i++) {
        // Same 24-bit-in-32-bit -> 16-bit PCM conversion used everywhere
        // else this project reads the INMP441 (PetInputs, the WAV
        // recorder) - see PetInputs.cpp for why >>16.
        _chunkBuf[_chunkFill++] = (int16_t)(i2sSamples[i] >> 16);
        if (_chunkFill >= _chunkSize) {
            wakenet_state_t state = _iface->detect(_model, _chunkBuf);
            if (state == WAKENET_DETECTED) _detected = true;
            _chunkFill = 0;
        }
    }
}

bool WakeWordDetector::detected() {
    bool d = _detected;
    _detected = false;
    return d;
}
