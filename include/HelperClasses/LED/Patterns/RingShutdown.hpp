#pragma once

#include "LedPatternInterface.hpp"

namespace UxModule
{
    // Fades the ring from a solid color down to black exactly once, then completes.
    // Intended to be played to completion (blocking) during a shutdown sequence via
    // LedPatternInterface::PlayBlocking(), since the device cuts power immediately
    // afterwards and cannot wait on the asynchronous LED task.
    class RingShutdown : public LedPatternInterface
    {
    public:
        RingShutdown(LedSegment segment, uint8_t r = 255, uint8_t g = 0, uint8_t b = 0)
            : LedPatternInterface(std::move(segment)), _r(r), _g(g), _b(b)
        {
            setAnimationLengthMS(2000);
        }

        bool iterateFrame() override
        {
            if (startTime == 0)
            {
                startTime = xTaskGetTickCount();
            }

            // Tick count is treated as milliseconds (1 ms tick), matching the other
            // time-based patterns in this codebase.
            size_t currMS = xTaskGetTickCount() - startTime;

            float brightness = 1.0f - (float)currMS / (float)animationMS;
            if (brightness < 0.0f)
            {
                brightness = 0.0f;
            }

            for (size_t i = 0; i < _segment.length(); i++)
            {
                _segment[i] = CRGB(_r * brightness, _g * brightness, _b * brightness);
            }

            return currMS >= animationMS;
        }

        void SetRegisteredPatternID(int patternID) { _RegisteredPatternId() = patternID; }
        static int RegisteredPatternID() { return _RegisteredPatternId(); }

    protected:
        static int &_RegisteredPatternId()
        {
            static int id = -1;
            return id;
        }

        uint8_t _r, _g, _b;

        const char *TAG = "Ring_Shutdown";
    };
}
