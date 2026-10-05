#pragma once

#include "LedPatternInterface.hpp"

namespace UxModule
{
    // Uses the LED ring to display a solid color
    class SolidRing : public LedPatternInterface
    {
    public:
        SolidRing(LedSegment segment)
            : LedPatternInterface(std::move(segment))
        {
            rOverride = 0;
            gOverride = 0;
            bOverride = 0;

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
        }

        bool iterateFrame()
        {
            ESP_LOGD(TAG, "SolidRing::iterateFrame");

            for (size_t i = 0; i < _segment.length(); i++)
            {
                _segment[i] = CRGB(rOverride, gOverride, bOverride);
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

        const char *TAG = "SolidRing";
    };
}
