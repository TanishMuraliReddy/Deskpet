#include "RoboFace.h"
#include <math.h>

namespace {

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float easeInOutQuad(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2) / 2.0f;
}

float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint16_t lerpColor565(uint16_t a, uint16_t b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (uint8_t)(ar + (br - ar) * t);
    uint8_t g = (uint8_t)(ag + (bg - ag) * t);
    uint8_t bl = (uint8_t)(ab + (bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Table of resting expressions. RoboFace blends _current toward whichever
// row is selected here rather than snapping to it directly.
FaceParams expressionParams(Expression e) {
    switch (e) {
        case Expression::HAPPY:
            // A kind, contented expression: soft curved closed eyes and a
            // shallow smile, rather than a hard squint or an exaggerated grin.
            return {0.98f, 0.20f, 1.0f, 0.03f, 0.0f, 0.82f, 0.36f, 0.0f, 0.72f, false, false, 0.0f};
        case Expression::SAD:
            // Rounded teardrop eyes: their high points face inward, paired
            // with a broad, gentle frown rather than a tiny sharp pout.
            return {0.98f, 1.02f, 1.0f, 0.08f, -0.82f, 0.0f, -0.70f, 0.0f, 0.90f, false, false, 0.0f};
        case Expression::ANGRY:
            return {0.9f, 0.75f, 0.85f, 0.0f, 0.9f, 0.15f, -0.4f, 0.0f, 0.45f, false, false, 0.0f};
        // Teardrop eyes (rounded bottom, gently angled top) with a separate
        // worried/sorry eyebrow arc above each (see drawBrow()) - the tilt
        // reads through the brow now, so the eye itself stays a clean
        // rounded shape rather than being sharply lid-masked the way
        // ANGRY/SUSPICIOUS are.
        case Expression::DISGRUNTLED:
            return {0.9f, 0.7f, 0.95f, 0.15f, 0.45f, 0.35f, -0.3f, 0.0f, 0.4f, false, false, 0.0f};
        case Expression::SURPRISED:
            return {1.15f, 1.25f, 1.05f, -0.15f, 0.0f, 0.0f, 0.0f, 0.85f, 0.4f, false, false, 0.0f};
        case Expression::SLEEPY:
            return {0.9f, 0.25f, 1.0f, 0.4f, 0.0f, 0.2f, -0.1f, 0.0f, 0.45f, false, false, 0.0f};
        // Eyes collapse to a thin closed line (eyeHeight), the mouth becomes
        // a small hollow "o" ring (see drawMouth), and drawSleepZs() floats
        // "Z Z Z" above the face while this is the active/target expression.
        case Expression::SLEEPING:
            return {1.0f, 0.14f, 1.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.35f, 0.19f, false, true, 0.0f};
        case Expression::SUSPICIOUS:
            return {0.85f, 0.5f, 0.7f, 0.05f, 0.3f, 0.4f, -0.2f, 0.0f, 0.4f, false, false, 0.0f};
        case Expression::ADORING:
            // A loving smile reads as a gentle, wide arc, not a pronounced
            // peak - lower mouthCurve + wider mouthWidth spreads the same
            // curve over more width, flattening it out.
            return {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.7f, 0.0f, 0.7f, true, false, 0.0f};
        case Expression::SCARED:
            return {1.3f, 1.3f, 1.1f, -0.1f, 0.0f, 0.0f, 0.0f, 0.5f, 0.3f, false, false, 0.0f};
        case Expression::VIBING:
            // Immersive vibe: relaxed closed ^_^ eyes and a warm smile.
            return {0.95f, 0.20f, 1.0f, 0.06f, 0.0f, 0.0f, 0.78f, 0.0f, 0.72f, false, false, 0.0f};
        case Expression::VIBING_ACTIVE:
            // Active vibe: open, bright eyes for the short beat accent.
            return {0.98f, 1.06f, 1.0f, -0.03f, 0.0f, 0.16f, 0.52f, 0.0f, 0.72f, false, false, 0.0f};
        case Expression::NEUTRAL:
        default:
            return {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.6f, false, false, 0.0f};
    }
}

FaceParams lerpParams(const FaceParams& a, const FaceParams& b, float t) {
    FaceParams out;
    out.eyeWidth = lerp(a.eyeWidth, b.eyeWidth, t);
    out.eyeHeight = lerp(a.eyeHeight, b.eyeHeight, t);
    out.eyeSpacing = lerp(a.eyeSpacing, b.eyeSpacing, t);
    out.eyeDroop = lerp(a.eyeDroop, b.eyeDroop, t);
    out.lidSlant = lerp(a.lidSlant, b.lidSlant, t);
    out.lidSquint = lerp(a.lidSquint, b.lidSquint, t);
    out.mouthCurve = lerp(a.mouthCurve, b.mouthCurve, t);
    out.mouthOpen = lerp(a.mouthOpen, b.mouthOpen, t);
    out.mouthWidth = lerp(a.mouthWidth, b.mouthWidth, t);
    out.browDroop = lerp(a.browDroop, b.browDroop, t);
    out.heartEyes = t >= 1.0f ? b.heartEyes : a.heartEyes;
    out.sleeping = t >= 1.0f ? b.sleeping : a.sleeping;
    return out;
}

// Geometry sized for the actual 128x128 SH1107 panel. The old dimensions
// were carried over from a 240px face design: wide/scared eyes plus a glance
// could extend past the left/right edge. This leaves a visible safety margin
// even for the largest expression and keeps the mouth away from the bottom.
constexpr float kBaseEyeW = 36.0f;
constexpr float kBaseEyeH = 42.0f;
constexpr float kBaseEyeSpacing = 56.0f;
constexpr float kBaseEyeRadius = 13.0f;
constexpr float kEyeDroopRange = 16.0f;
constexpr float kMouthBaseWidth = 58.0f;
constexpr float kMouthCurveAmplitude = 15.0f;
constexpr float kMouthOpenMaxHeight = 26.0f;
constexpr float kLookRangeX = 8.0f;
constexpr float kLookRangeY = 5.0f;

} // namespace

RoboFace::RoboFace(Adafruit_GFX* gfx, int16_t width, int16_t height)
    : _gfx(gfx), _w(width), _h(height),
      _eyeColor(0xFFFF), _bgColor(0x0000),
      _autonomous(true),
      _nextBlinkAtMs(0), _blinkStartMs(0), _blinking(false),
      _lookX(0), _lookY(0), _lookTargetX(0), _lookTargetY(0),
      _manualLookUntilMs(0), _nextSaccadeAtMs(0),
      _talking(false), _mouthOverrideActive(false), _mouthOverrideAmount(0.0f),
      _lastUpdateMs(0) {
    _current = expressionParams(Expression::NEUTRAL);
    _target = _current;
    _transitionStart = _current;
    _transitionStartMs = 0;
    _transitionDurationMs = 1;
}

void RoboFace::begin() {
    _lastUpdateMs = millis();
    _nextBlinkAtMs = _lastUpdateMs + random(2000, 6000);
    _nextSaccadeAtMs = _lastUpdateMs + random(900, 1900);
    render();
}

void RoboFace::setColors(uint16_t eyeColor, uint16_t bgColor) {
    _eyeColor = eyeColor;
    _bgColor = bgColor;
}

void RoboFace::setExpression(Expression e, float transitionSeconds) {
    _transitionStart = _current;
    _target = expressionParams(e);
    _transitionStartMs = millis();
    _transitionDurationMs = (uint32_t)(transitionSeconds * 1000.0f);
    if (_transitionDurationMs == 0) _transitionDurationMs = 1;
}

void RoboFace::setTalking(bool on) {
    _talking = on;
    if (on) _mouthOverrideActive = false;
}

void RoboFace::setMouthOverride(bool active, float amount) {
    _mouthOverrideActive = active;
    _mouthOverrideAmount = clampf(amount, 0.0f, 1.0f);
    if (active) _talking = false;
}

void RoboFace::lookAt(float nx, float ny, uint32_t holdMs) {
    _lookTargetX = clampf(nx, -1.0f, 1.0f);
    _lookTargetY = clampf(ny, -1.0f, 1.0f);
    _manualLookUntilMs = millis() + holdMs;
}

void RoboFace::blinkOnce() {
    if (!_blinking) {
        _blinking = true;
        _blinkStartMs = millis();
    }
}

void RoboFace::setAutonomous(bool on) {
    _autonomous = on;
}

void RoboFace::update() {
    uint32_t now = millis();
    float dt = (now - _lastUpdateMs) / 1000.0f;
    if (dt <= 0.0f) return;
    _lastUpdateMs = now;
    tick(dt);
    render();
}

void RoboFace::tick(float dtSeconds) {
    uint32_t now = millis();

    // Expression blend
    float t = (now - _transitionStartMs) / (float)_transitionDurationMs;
    t = clampf(t, 0.0f, 1.0f);
    _current = lerpParams(_transitionStart, _target, easeInOutQuad(t));

    // Autonomous blink scheduling
    if (_autonomous && !_blinking && now >= _nextBlinkAtMs) {
        _blinking = true;
        _blinkStartMs = now;
    }
    if (_blinking && now - _blinkStartMs >= kBlinkDurationMs) {
        _blinking = false;
        _nextBlinkAtMs = now + random(2000, 6000);
    }

    // Gaze: autonomous saccades unless a manual lookAt() is still holding
    if (_autonomous && now >= _manualLookUntilMs && now >= _nextSaccadeAtMs) {
        // Avoid tiny near-center target changes: they existed, but were too
        // subtle to read as a living idle glance on the small OLED.
        const float side = random(0, 2) ? 1.0f : -1.0f;
        _lookTargetX = side * (random(55, 101) / 100.0f);
        _lookTargetY = (random(-35, 36) / 100.0f);
        _nextSaccadeAtMs = now + random(900, 1900);
    }
    float lookRate = clampf(dtSeconds * 6.0f, 0.0f, 1.0f);
    _lookX = lerp(_lookX, _lookTargetX, lookRate);
    _lookY = lerp(_lookY, _lookTargetY, lookRate);
}

float RoboFace::blinkFactor() const {
    if (!_blinking) return 1.0f;
    uint32_t elapsed = millis() - _blinkStartMs;
    float phase = (float)elapsed / (float)kBlinkDurationMs; // 0..1 over the whole blink
    // Close over the first half, reopen over the second half.
    float closeAmount = phase < 0.5f ? (phase * 2.0f) : (2.0f - phase * 2.0f);
    return 1.0f - clampf(closeAmount, 0.0f, 1.0f);
}

void RoboFace::render() {
    _gfx->fillScreen(_bgColor);

    float openness = blinkFactor();
    int16_t cx = _w / 2;
    int16_t cy = _h / 2;
    float spacingPx = kBaseEyeSpacing * _current.eyeSpacing;
    float lookOffX = _lookX * kLookRangeX;
    float lookOffY = _lookY * kLookRangeY;
    float droopPx = _current.eyeDroop * kEyeDroopRange;

    int16_t leftX = (int16_t)(cx - spacingPx / 2 + lookOffX);
    int16_t rightX = (int16_t)(cx + spacingPx / 2 + lookOffX);
    int16_t eyeY = (int16_t)(cy - 8 + droopPx + lookOffY);

    if (_current.heartEyes) {
        drawHeartEye(leftX, eyeY, _eyeColor);
        drawHeartEye(rightX, eyeY, _eyeColor);
    } else {
        drawEye(leftX, eyeY, true, openness, _eyeColor);
        drawEye(rightX, eyeY, false, openness, _eyeColor);
        if (_current.browDroop > 0.01f) {
            drawBrow(leftX, eyeY, true, _current.browDroop, _eyeColor);
            drawBrow(rightX, eyeY, false, _current.browDroop, _eyeColor);
        }
    }

    drawMouth(cx + (int16_t)(lookOffX * 0.3f), (int16_t)(cy + 38), _eyeColor);

    if (_current.sleeping) {
        drawSleepZs(rightX + 4, eyeY - 26, _eyeColor);
    }

    // If _gfx is a buffered target (e.g. Arduino_Canvas), the caller is
    // responsible for flushing it to the physical panel after update().
}

void RoboFace::drawEye(int16_t centerX, int16_t centerY, bool isLeftEye,
                        float openness, uint16_t color) const {
    float w = kBaseEyeW * _current.eyeWidth;

    // Nearly/fully shut (blink trough, SLEEPY, SLEEPING) reads as a soft
    // curved line far better than a squashed box - closer to how a real
    // closed eye (or the reference face's resting expression) looks.
    float openFrac = clampf(openness, 0.0f, 1.0f) * clampf(_current.eyeHeight, 0.0f, 2.0f);
    if (openFrac < 0.30f) {
        float bend = clampf(_current.lidSquint - fmaxf(0.0f, -_current.lidSlant), -1.0f, 1.0f);
        drawEyeArc(centerX, centerY, w * 0.55f, bend, color);
        return;
    }

    float h = kBaseEyeH * _current.eyeHeight * clampf(openness, 0.06f, 1.0f);
    int16_t x = (int16_t)(centerX - w / 2);
    int16_t y = (int16_t)(centerY - h / 2);
    // A rounder "squircle" (radius scales with the eye's own size instead
    // of a small fixed constant) reads closer to a friendly robot-face eye
    // than a barely-rounded box.
    int16_t radius = (int16_t)(fminf(w, h) * 0.48f);

    _gfx->fillRoundRect(x, y, (int16_t)w, (int16_t)h, radius, color);

    if (fabsf(_current.lidSlant) <= 0.01f && _current.lidSquint > 0.5f) {
        // Happy: no slant, strong squint - taper both top corners into a
        // shallow peak so the eye reads as a "^_^" arc instead of masking
        // a flat bar in from below.
        float dropH = h * 0.75f * _current.lidSquint;
        cutTopCorner(x, y, w, dropH, 0.42f, /*towardRight=*/true);
        cutTopCorner((int16_t)(x + w), y, w, dropH, 0.42f, /*towardRight=*/false);
    } else if (_current.lidSquint > 0.01f) {
        // Otherwise (suspicious/disgruntled/slight squint alongside a
        // slant): mask the lower portion with a rounded bar in the
        // background color, rising up into the eye from below.
        float squintH = h * 0.6f * _current.lidSquint;
        int16_t sy = (int16_t)(centerY + h / 2 - squintH);
        _gfx->fillRoundRect(x - 2, sy, (int16_t)w + 4, (int16_t)squintH + radius,
                             radius, _bgColor);
    }

    // Top lid slant: positive lidSlant (angry) masks the inner-top corner;
    // negative (sad) masks the outer-top corner. "Inner" = nearest the nose.
    if (fabsf(_current.lidSlant) > 0.01f) {
        bool maskInner = _current.lidSlant > 0.0f;
        bool maskLeftCorner = isLeftEye ? maskInner : !maskInner;
        float dropH = h * 0.75f * fabsf(_current.lidSlant);
        int16_t triX = maskLeftCorner ? x : (int16_t)(x + w);
        cutTopCorner(triX, y, w, dropH, 0.6f, maskLeftCorner);
    }
}

void RoboFace::drawEyeArc(int16_t centerX, int16_t centerY, float halfWidth,
                           float bend, uint16_t color) const {
    const uint8_t steps = 22;
    const float amplitude = 8.0f;
    float prevX = centerX - halfWidth, prevY = centerY;
    for (uint8_t i = 1; i <= steps; i++) {
        float t = i / (float)steps;
        float x = lerp(-halfWidth, halfWidth, t);
        // Same eased sin^2 bump drawMouth() uses for its closed-mouth
        // curve, so closed eyes and mouth read as the same "hand".
        float bump = sinf(3.14159265f * t);
        float y = centerY - bend * amplitude * bump * bump;
        stampLine(prevX, prevY, centerX + x, y, 1, 2, color);
        prevX = centerX + x;
        prevY = y;
    }
}

void RoboFace::cutTopCorner(int16_t anchorX, int16_t topY, float w, float dropH,
                             float widthFrac, bool towardRight) const {
    // Approximates a diagonal mask with stacked horizontal bars, each a
    // little shorter than the last, so the cut reads as a slanted edge.
    const uint8_t bars = 6;
    for (uint8_t i = 0; i < bars; i++) {
        float rowT = i / (float)(bars - 1);           // 0 at top .. 1 lower
        int16_t rowY = (int16_t)(topY + rowT * dropH);
        int16_t rowW = (int16_t)(w * widthFrac * (1.0f - rowT));
        int16_t rowX = towardRight ? anchorX : (int16_t)(anchorX - rowW);
        _gfx->fillRect(rowX, rowY, rowW, (int16_t)(dropH / bars) + 2, _bgColor);
    }
}

void RoboFace::drawHeartEye(int16_t centerX, int16_t centerY, uint16_t color) const {
    // Classic parametric heart curve (MathWorld "Heart Curve"), sampled and
    // filled as a polygon. This replaces an earlier 2-circles-plus-triangle
    // build that looked faceted - the triangle's flat top edge cut across
    // the circles at an arbitrary height instead of meeting them smoothly,
    // leaving a visible notch on each side.
    const uint8_t kSamples = 40;
    float sampleX[kSamples];
    float sampleY[kSamples];
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;

    for (uint8_t i = 0; i < kSamples; i++) {
        float t = (float)i / kSamples * 6.28318530718f;
        float sx = 16.0f * sinf(t) * sinf(t) * sinf(t);
        float sy = 13.0f * cosf(t) - 5.0f * cosf(2.0f * t) - 2.0f * cosf(3.0f * t) - cosf(4.0f * t);
        sampleX[i] = sx;
        sampleY[i] = sy;
        minX = fminf(minX, sx); maxX = fmaxf(maxX, sx);
        minY = fminf(minY, sy); maxY = fmaxf(maxY, sy);
    }

    float targetW = kBaseEyeW * _current.eyeWidth * 0.8f;
    float targetH = kBaseEyeH * 0.75f;
    float scale = fminf(targetW / (maxX - minX), targetH / (maxY - minY));
    float midX = (minX + maxX) / 2.0f;
    float midY = (minY + maxY) / 2.0f;

    int16_t px[kSamples];
    int16_t py[kSamples];
    for (uint8_t i = 0; i < kSamples; i++) {
        px[i] = (int16_t)(centerX + (sampleX[i] - midX) * scale);
        // The curve's y grows upward mathematically; pixel space grows
        // downward, so the sign flips here.
        py[i] = (int16_t)(centerY - (sampleY[i] - midY) * scale);
    }

    fillPolygon(px, py, kSamples, color);
}

void RoboFace::drawBrow(int16_t centerX, int16_t eyeCenterY, bool isLeftEye,
                         float droop, uint16_t color) const {
    float halfWidth = kBaseEyeW * _current.eyeWidth * 0.5f * 0.88f;
    float baseY = eyeCenterY - (kBaseEyeH * _current.eyeHeight) / 2.0f - 7.0f;
    float outerX = isLeftEye ? centerX - halfWidth : centerX + halfWidth;
    float innerX = isLeftEye ? centerX + halfWidth : centerX - halfWidth;
    float outerY = baseY - 4.0f * droop;
    float innerY = baseY + 7.0f * droop;

    const uint8_t steps = 8;
    float prevX = outerX, prevY = outerY;
    for (uint8_t i = 1; i <= steps; i++) {
        float t = i / (float)steps;
        float x = lerp(outerX, innerX, t);
        float base = lerp(outerY, innerY, t);
        float arch = -3.5f * droop * sinf(3.14159265f * t);
        float y = base + arch;
        stampLine(prevX, prevY, x, y, 2, 3, color);
        prevX = x;
        prevY = y;
    }
}

void RoboFace::fillPolygon(const int16_t* px, const int16_t* py, uint8_t count,
                            uint16_t color) const {
    int16_t minY = py[0], maxY = py[0];
    for (uint8_t i = 1; i < count; i++) {
        minY = min(minY, py[i]);
        maxY = max(maxY, py[i]);
    }

    for (int16_t y = minY; y <= maxY; y++) {
        int16_t crossings[8];
        uint8_t n = 0;
        for (uint8_t i = 0; i < count; i++) {
            uint8_t j = (i + 1) % count;
            int16_t y0 = py[i], y1 = py[j];
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                float t = (float)(y - y0) / (float)(y1 - y0);
                int16_t x = (int16_t)(px[i] + t * (px[j] - px[i]));
                if (n < 8) crossings[n++] = x;
            }
        }
        // Insertion sort - n is always small (this curve crosses any
        // scanline at most 4 times, near its top-center notch).
        for (uint8_t i = 1; i < n; i++) {
            int16_t key = crossings[i];
            int8_t k = (int8_t)i - 1;
            while (k >= 0 && crossings[k] > key) {
                crossings[k + 1] = crossings[k];
                k--;
            }
            crossings[k + 1] = key;
        }
        for (uint8_t i = 0; i + 1 < n; i += 2) {
            _gfx->drawFastHLine(crossings[i], y, crossings[i + 1] - crossings[i] + 1, color);
        }
    }
}

void RoboFace::drawMouth(int16_t centerX, int16_t centerY, uint16_t color) const {
    float mouthOpen = _mouthOverrideActive ? _mouthOverrideAmount : _current.mouthOpen;

    if (_talking && !_mouthOverrideActive) {
        float t = millis() / 1000.0f;
        float flutter = 0.5f + 0.5f * sinf(t * 11.0f) * 0.6f + 0.4f * sinf(t * 23.0f + 1.7f);
        mouthOpen = clampf(fmaxf(mouthOpen, 0.15f) * flutter + 0.1f, 0.08f, 1.0f);
    }

    float halfWidth = (kMouthBaseWidth * _current.mouthWidth) / 2.0f;

    if (mouthOpen > 0.05f) {
        int16_t h = (int16_t)(kMouthOpenMaxHeight * mouthOpen);
        int16_t w = (int16_t)(halfWidth * 2.0f * (1.0f - 0.3f * mouthOpen));
        if (_current.sleeping) {
            // Sleeping mouth is a small hollow "o" ring instead of filled.
            int16_t outerR = (int16_t)(fmaxf(h, w) / 2.0f);
            _gfx->fillCircle(centerX, centerY, outerR, color);
            _gfx->fillCircle(centerX, centerY, (int16_t)(outerR * 0.45f), _bgColor);
        } else {
            // Open mouth: a filled ellipse whose height tracks aperture.
            _gfx->fillRoundRect(centerX - w / 2, centerY - h / 2, w, h,
                                 (int16_t)fminf(h, w) / 3, color);
        }
        return;
    }

    // Closed mouth: stroke a curve from corner to corner. mouthCurve>0 bows
    // upward at the ends (smile), <0 bows downward at the ends (frown).
    const uint8_t steps = 36;
    float prevX = centerX - halfWidth, prevY = centerY;
    for (uint8_t i = 1; i <= steps; i++) {
        float tt = i / (float)steps;
        float x = lerp(-halfWidth, halfWidth, tt);
        // sin(pi*t)^2 is 0 at both corners *with zero slope there* and peaks
        // at the middle, so the curve eases flat into the corners instead of
        // arriving at a steep angle - a plain parabola has nonzero slope at
        // t=0/1, which read as sharp, pointed corners rather than a soft
        // curl. mouthCurve>0 (smile) pushes the middle down into a "U";
        // mouthCurve<0 (frown) pulls it up into an "n".
        float bump = sinf(3.14159265f * tt);
        float y = centerY + (_current.mouthCurve * kMouthCurveAmplitude * bump * bump);
        stampLine(prevX, prevY, centerX + x, y, 1, 2, color);
        prevX = centerX + x;
        prevY = y;
    }
}

void RoboFace::drawSleepZs(int16_t anchorX, int16_t anchorY, uint16_t color) const {
    const float kCycleMs = 2200.0f;
    const uint8_t kZCount = 3;
    uint32_t now = millis();

    for (uint8_t i = 0; i < kZCount; i++) {
        float phase = fmodf((now / kCycleMs) + (i / (float)kZCount), 1.0f);
        uint8_t textSize = phase < 0.34f ? 1 : (phase < 0.67f ? 2 : 3);
        int16_t x = (int16_t)(anchorX + phase * 30.0f + i * 6);
        int16_t y = (int16_t)(anchorY - phase * 32.0f - i * 4);

        // Fade in over the first 15% of the cycle, fade out over the last
        // 25%, so each "Z" drifts up and to the right without popping.
        float fadeIn = clampf(phase / 0.15f, 0.0f, 1.0f);
        float fadeOut = clampf((1.0f - phase) / 0.25f, 0.0f, 1.0f);
        uint16_t zColor = lerpColor565(_bgColor, color, fminf(fadeIn, fadeOut));

        _gfx->setTextSize(textSize);
        _gfx->setTextColor(zColor);
        _gfx->setCursor(x, y);
        _gfx->print('Z');
    }
}

void RoboFace::stampLine(float x0, float y0, float x1, float y1, uint8_t steps,
                          uint8_t thickness, uint16_t color) const {
    for (uint8_t i = 0; i <= steps; i++) {
        float t = i / (float)steps;
        int16_t x = (int16_t)lerp(x0, x1, t);
        int16_t y = (int16_t)lerp(y0, y1, t);
        _gfx->fillCircle(x, y, thickness / 2, color);
    }
}
