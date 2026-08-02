#pragma once
#include <Arduino.h>

// Real clock via NTP - only needs WiFi, no RTC module required.
namespace TimeConfig {
    constexpr long kGmtOffsetSec = -5 * 3600; // US Eastern - edit for your timezone
    constexpr int kDaylightOffsetSec = 3600;
}

struct TimeInfo {
    bool ok;
    String sentence; // full descriptive text, or an error message if !ok
    String value;    // compact reading for the on-screen data card - empty if !ok
};

void beginTimeSync(); // call once, after WiFi connects
TimeInfo fetchTime();  // sentence "It's 03:45 PM.", value "03:45 PM"
TimeInfo fetchDate();  // sentence "It's Thursday, July 09.", value "Thu Jul 09"
