#pragma once
#include <Adafruit_GFX.h>

// Named resting expressions. Each maps to a FaceParams entry in RoboFace.cpp.
enum class Expression : uint8_t {
    NEUTRAL,
    HAPPY,
    SAD,
    ANGRY,
    DISGRUNTLED, // mild, grumpy annoyance - short of full ANGRY
    SURPRISED,
    SLEEPY,      // drowsy, eyes going heavy
    SLEEPING,    // fully asleep - closed-line eyes, "o" mouth, floating "Z Z Z"
    SUSPICIOUS,
    ADORING,     // heart eyes
    SCARED,
    VIBING,          // relaxed closed-eye groove
    VIBING_ACTIVE,   // brief wide-eyed beat hit
    EXPRESSION_COUNT
};

// Interpolated shape state for the face. RoboFace blends the current values
// toward a target Expression's FaceParams every frame instead of snapping,
// so expression changes read as an animation instead of a cut.
struct FaceParams {
    float eyeWidth;    // relative eye width, 1.0 = default
    float eyeHeight;   // relative eye height, 1.0 = default (before blink is applied)
    float eyeSpacing;  // relative gap between the two eyes, 1.0 = default
    float eyeDroop;    // vertical eye offset: -1 raised .. 0 neutral .. +1 lowered
    // lidSlant sign convention: positive = angry (inner-top corner, the side
    // nearest the nose, is masked down); negative = sad (outer-top corner is
    // masked down instead). Mirrored automatically between left/right eye.
    float lidSlant;
    float lidSquint;   // 0..1, bottom lid rise (happy/suspicious squint)
    float mouthCurve;  // -1 frown .. 0 flat .. +1 smile
    float mouthOpen;   // 0..1 mouth aperture (surprised/talking/scared)
    float mouthWidth;  // relative mouth width, 1.0 = default
    bool heartEyes;    // overrides normal eye shape with heart eyes when true
    bool sleeping;     // draws the mouth as a ring instead of filled, and
                       // floats "Z Z Z" above the face, when true
    // 0 = no eyebrow drawn (most expressions convey tilt via lidSlant on the
    // eye itself instead). >0 draws a separate worried/sorry eyebrow arc
    // above the eye - higher on the outer end, drooping toward the inner
    // end - currently just DISGRUNTLED.
    float browDroop;
};

class RoboFace {
public:
    // gfx must already be sized to the drawable face area (e.g. a
    // GFXcanvas16 offscreen buffer you later blit to the GC9A01 panel).
    RoboFace(Adafruit_GFX* gfx, int16_t width, int16_t height);

    void begin();

    // Smoothly blends toward the named expression over transitionSeconds.
    void setExpression(Expression e, float transitionSeconds = 0.35f);

    // Idle talking mouth flutter. Disable and call setMouthOverride() instead
    // if you want to drive the mouth from a real audio envelope.
    void setTalking(bool on);
    void setMouthOverride(bool active, float amount = 0.0f);

    // Manual gaze target in normalized socket space, each axis -1..1.
    // Autonomous saccades resume automatically after holdMs.
    void lookAt(float nx, float ny, uint32_t holdMs = 1500);

    void blinkOnce();
    void setAutonomous(bool on); // idle blink + look-around behavior

    // Defaults are a white face on a black background.
    void setColors(uint16_t eyeColor, uint16_t bgColor);

    // Call every loop iteration. Non-blocking; internally timed via millis().
    void update();

private:
    Adafruit_GFX* _gfx;
    int16_t _w, _h;
    uint16_t _eyeColor;
    uint16_t _bgColor;

    FaceParams _current;
    FaceParams _target;
    FaceParams _transitionStart;
    uint32_t _transitionStartMs;
    uint32_t _transitionDurationMs;

    bool _autonomous;

    // Blink state
    uint32_t _nextBlinkAtMs;
    uint32_t _blinkStartMs;
    bool _blinking;
    static const uint32_t kBlinkDurationMs = 260;

    // Saccade / gaze state
    float _lookX, _lookY;         // current rendered gaze offset, -1..1
    float _lookTargetX, _lookTargetY;
    uint32_t _manualLookUntilMs;
    uint32_t _nextSaccadeAtMs;

    // Talking state
    bool _talking;
    bool _mouthOverrideActive;
    float _mouthOverrideAmount;

    uint32_t _lastUpdateMs;

    void tick(float dtSeconds);
    void render();
    float blinkFactor() const; // 1.0 = fully open, 0.0 = fully closed

    void drawEye(int16_t centerX, int16_t centerY, bool isLeftEye,
                 float openness, uint16_t color) const;
    // Dedicated, rounded teardrop eye used by the Sad expression.  This is
    // intentionally separate from the generic lid-mask eye so Sad keeps its
    // distinctive inward-pointing silhouette on the small OLED.
    void drawSadEye(int16_t centerX, int16_t centerY, bool isLeftEye,
                     uint16_t color) const;
    // Soft closed-eye curve used when an eye is nearly/fully shut (blink,
    // SLEEPY, SLEEPING): bend>0 arches upward ("^_^"), bend<0 droops (sad).
    void drawEyeArc(int16_t centerX, int16_t centerY, float halfWidth,
                     float bend, uint16_t color) const;
    // Stamps a diagonal wedge of background color into one top corner of an
    // eye, widest at the corner and tapering to a point dropH below it -
    // shared by the angry/sad slant cut and the happy top-corner taper.
    void cutTopCorner(int16_t anchorX, int16_t topY, float w, float dropH,
                       float widthFrac, bool towardRight) const;
    void drawHeartEye(int16_t centerX, int16_t centerY, uint16_t color) const;
    void drawBrow(int16_t centerX, int16_t eyeCenterY, bool isLeftEye,
                   float droop, uint16_t color) const;
    void drawMouth(int16_t centerX, int16_t centerY, uint16_t color) const;
    void drawSleepZs(int16_t anchorX, int16_t anchorY, uint16_t color) const;
    void stampLine(float x0, float y0, float x1, float y1, uint8_t steps,
                    uint8_t thickness, uint16_t color) const;
    void fillPolygon(const int16_t* px, const int16_t* py, uint8_t count,
                      uint16_t color) const;
};
