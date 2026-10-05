#pragma once

#include "LedPatternInterface.hpp"

namespace UxModule
{
    // Fades in entire LED ring then fades out
    class RingPulse : public LedPatternInterface
    {
    public:
        RingPulse(LedSegment segment)
            : LedPatternInterface(std::move(segment))
        {
            rOverride = 0;
            gOverride = 0;
            bOverride = 0;
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
            ESP_LOGV(TAG, "Ring Pulse");

            if (startTime == 0)
            {
                startTime = xTaskGetTickCount();
            }

            size_t currMS = xTaskGetTickCount() - startTime;

            // Determine brightness this frame
            // First half of animation fades in, second half fades out
            float brightness = 1.0;
            if (currMS < animationMS / 2)
            {
                brightness = (float)currMS / (float)(animationMS / 2);
            }
            else
            {
                brightness = 1.0 - (float)(currMS - (animationMS / 2)) / (float)(animationMS / 2);
            }

            // use override colors if set
            CRGB color;
            if (rOverride != 0 || gOverride != 0 || bOverride != 0)
            {
                color = CRGB(rOverride * brightness, gOverride * brightness, bOverride * brightness);
            }
            else
            {
                color = CRGB(ThemeColor().r * brightness, ThemeColor().g * brightness, ThemeColor().b * brightness);
            }

            for (size_t i = 0; i < _segment.length(); i++)
            {
                _segment[i] = color;
            }

            if (currMS >= animationMS)
            {
                clearPattern();
                return true;
            }
            else
            {
                return false;
            }
        }

        // Only claims the ring while a pulse is running; clearPattern() at the
        // end of each loop zeroes startTime.
        bool isActive() const override { return startTime != 0; }

        void SetRegisteredPatternID(int patternID) { _RegisteredPatternId() = patternID; }
        static int RegisteredPatternID() { return _RegisteredPatternId(); }

    protected:
        static int &_RegisteredPatternId()
        {
            static int id = -1;
            return id;
        }

        uint8_t rOverride, gOverride, bOverride;

        const char *TAG = "Ring_Pulse";
    };
}
