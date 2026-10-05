#pragma once

#include <cmath>
#include "LedPatternInterface.hpp"

namespace UxModule
{
    // A pattern used to point a direction with the LED ring.
    // The angle is degrees from the beginning index of the ring is supplied for the direction.
    // The sharpness of the point is controlled by the fadeDegrees parameter.
    // This can be used as a compass or a scrolling indicator in menus.
    // This pattern is not animated and will be called when updates are needed.
    class RingPoint : public LedPatternInterface
    {
    public:

        RingPoint(LedSegment segment)
            : LedPatternInterface(std::move(segment))
        {
            rOverride = 0;
            gOverride = 0;
            bOverride = 0;

            fadeDegrees = 0;
            directionDegrees = 0;

            setAnimationLengthTicks(1);
        }

        void configurePattern(JsonDocument &config)
        {
            if (!config["rOverride"].isNull())
            {
                rOverride = config["rOverride"];
            }

            if (!config["gOverride"].isNull())
            {
                gOverride = config["gOverride"];
            }

            if (!config["bOverride"].isNull())
            {
                bOverride = config["bOverride"];
            }

            if (!config["fadeDegrees"].isNull())
            {
                float fade = config["fadeDegrees"];
                fadeDegrees = (fade > 0.0f) ? fade : 0.0f;
            }

            if (!config["directionDegrees"].isNull())
            {
                // Callers pass bearings straight from navigation math, which can
                // be negative or >= 360 (e.g. a "no fix" sentinel run through
                // GetBearing). The wrap in _GetLEDBrightness assumes [0, 360).
                float direction = fmodf(config["directionDegrees"].as<float>(), 360.0f);
                if (direction < 0.0f)
                {
                    direction += 360.0f;
                }
                directionDegrees = direction;
            }
        }

        bool iterateFrame()
        {
            ESP_LOGD(TAG, "RingPoint::iterateFrame");

            _segment.clear();

            CRGB outColor;
            if (rOverride == 0 && gOverride == 0 && bOverride == 0)
            {
                outColor = ThemeColor();
            }
            else
            {
                outColor = CRGB(rOverride, gOverride, bOverride);
            }

            for (size_t i = 0; i < _segment.length(); i++)
            {
                float brightness = _GetLEDBrightness(i);
                _segment[i] = CRGB(outColor.r * brightness, outColor.g * brightness, outColor.b * brightness);
            }

            return true;
        }

        void SetRegisteredPatternID(int patternID) { _RegisteredPatternId() = patternID; }
        static int RegisteredPatternID() { return _RegisteredPatternId(); }

    protected:
        static int &_RegisteredPatternId()
        {
            static int id = -1;
            return id;
        }

        uint8_t rOverride;
        uint8_t gOverride;
        uint8_t bOverride;

        float fadeDegrees;
        float directionDegrees;

        const char *TAG = "RingPoint";

        float _GetLEDBrightness(size_t segIdx)
        {
            const float ledSpacing = 360.0f / (float)_segment.length();
            float angle = (float)segIdx * ledSpacing;

            float angleDiff = fabsf(angle - directionDegrees);

            if (angleDiff > 180.0f)
            {
                angleDiff = 360.0f - angleDiff;
            }

            // No fade configured: a hard point on the single nearest LED rather
            // than a divide by zero.
            if (fadeDegrees <= 0.0f)
            {
                return (angleDiff <= ledSpacing / 2.0f) ? 1.0f : 0.0f;
            }

            if (angleDiff > fadeDegrees)
            {
                return 0.0f;
            }

            float brightness = 1.0f - (angleDiff / fadeDegrees);
            if (brightness < 0.0f)
            {
                brightness = 0.0f;
            }
            else if (brightness > 1.0f)
            {
                brightness = 1.0f;
            }
            return brightness;
        }
    };
}
