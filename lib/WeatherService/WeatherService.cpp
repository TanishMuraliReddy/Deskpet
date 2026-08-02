#include "WeatherService.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {

// Open-Meteo reports WMO weather codes; this collapses them to a short
// phrase. https://open-meteo.com/en/docs#weathervariables
const char* describeWeatherCode(int code) {
    if (code == 0) return "clear";
    if (code <= 3) return "partly cloudy";
    if (code == 45 || code == 48) return "foggy";
    if (code <= 57) return "drizzling";
    if (code <= 67) return "rainy";
    if (code <= 77) return "snowy";
    if (code <= 82) return "showery";
    if (code <= 86) return "snowy";
    if (code <= 99) return "stormy";
    return "doing something unusual";
}

WeatherInfo fail(const char* message) {
    return {false, String(message), "", ""};
}

} // namespace

WeatherInfo fetchWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        return fail("I'm not on WiFi, so I can't check the weather.");
    }

    char url[192];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&temperature_unit=fahrenheit",
             WeatherConfig::kLatitude, WeatherConfig::kLongitude);

    HTTPClient http;
    http.begin(url);
    int status = http.GET();
    if (status != 200) {
        http.end();
        return fail("I couldn't reach the weather service.");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        return fail("The weather service sent something I couldn't read.");
    }

    float tempF = doc["current"]["temperature_2m"] | 0.0f;
    int code = doc["current"]["weather_code"] | -1;
    const char* condition = describeWeatherCode(code);

    WeatherInfo info;
    info.ok = true;
    char sentence[64];
    snprintf(sentence, sizeof(sentence), "It's %.0f F and %s.", tempF, condition);
    info.sentence = String(sentence);
    // No degree symbol - the stock Adafruit_GFX font's mapping for it isn't
    // reliable across displays, so this sticks to plain ASCII.
    char value[16];
    snprintf(value, sizeof(value), "%.0fF", tempF);
    info.value = String(value);
    info.caption = String(condition);
    return info;
}
