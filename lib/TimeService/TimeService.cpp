#include "TimeService.h"
#include <WiFi.h>
#include <time.h>

void beginTimeSync() {
    configTime(TimeConfig::kGmtOffsetSec, TimeConfig::kDaylightOffsetSec,
               "pool.ntp.org", "time.nist.gov");
}

namespace {

bool getLocal(struct tm& info) {
    if (WiFi.status() != WL_CONNECTED) return false;
    return getLocalTime(&info, 2000);
}

TimeInfo fail(const char* message) {
    return {false, String(message), ""};
}

} // namespace

TimeInfo fetchTime() {
    struct tm info;
    if (!getLocal(info)) return fail("I don't know the time yet - still syncing.");

    TimeInfo out;
    out.ok = true;
    char buf[32];
    strftime(buf, sizeof(buf), "It's %I:%M %p.", &info);
    out.sentence = String(buf);
    strftime(buf, sizeof(buf), "%I:%M %p", &info);
    out.value = String(buf);
    return out;
}

TimeInfo fetchDate() {
    struct tm info;
    if (!getLocal(info)) return fail("I don't know the date yet - still syncing.");

    TimeInfo out;
    out.ok = true;
    char buf[48];
    strftime(buf, sizeof(buf), "It's %A, %B %d.", &info);
    out.sentence = String(buf);
    strftime(buf, sizeof(buf), "%a %b %d", &info);
    out.value = String(buf);
    return out;
}
