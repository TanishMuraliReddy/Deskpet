#pragma once
#include <Arduino.h>

// Real weather via Open-Meteo (https://open-meteo.com) - free, no API key.
// Only needs the WiFi this ESP32 already has, so unlike tilt/noise this
// isn't a stub: fill in your coordinates below and it works today.
namespace WeatherConfig {
    constexpr float kLatitude = 34.0007f;   // Cumming, GA - edit to your location
    constexpr float kLongitude = -84.1402f;
}

struct WeatherInfo {
    bool ok;
    String sentence; // full descriptive text, or an error message if !ok
    String value;    // compact reading for the on-screen data card, e.g. "72F" - empty if !ok
    String caption;  // short condition word, e.g. "partly cloudy" - empty if !ok
};

WeatherInfo fetchWeather();
