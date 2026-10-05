#pragma once

#include "LedPatternInterface.hpp"

namespace UxModule
{
    // Uses the LED ring to display scrolling progress
    class ScrollWheel : public LedPatternInterface
    {
    public:
        ScrollWheel(LedSegment segment)
            : LedPatternInterface(std::move(segment))
        {
            numItems = -1;
            currItem = -1;

            setAnimationLengthTicks(1);
        }

        void configurePattern(JsonDocument &config)
        {
            if (!config["numItems"].isNull())
            {
                numItems = config["numItems"];
            }

            if (!config["currItem"].isNull())
            {
                currItem = config["currItem"];
            }
        }

        bool iterateFrame()
        {
            if (numItems == -1 || currItem == -1)
            {
                ESP_LOGW(TAG, "ScrollWheel::iterateFrame: Invalid configuration, cannot iterate frame");
                return true;
            }

            ESP_LOGV(TAG, "ScrollWheel::iterateFrame numItems: %d currItem: %d", numItems, currItem);

            for (size_t i = 0; i < _segment.length(); i++)
            {
                float ledBrightness = _CalculateLEDPointBrightness(i);
                _segment[i] = CRGB((ThemeColor().r) * ledBrightness, (ThemeColor().g) * ledBrightness, (ThemeColor().b) * ledBrightness);
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

        int numItems;
        int currItem;

        const char *TAG = "ScrollWheel";

        __attribute__((noinline)) float _CalculateLEDPointBrightness(size_t segIdx)
        {
            if (numItems == -1 || currItem == -1)
            {
                return 0.0f;
            }

            float targetAngle = ((float)currItem / numItems) * 360.0;

            // float fadeDegrees = max((360.0f / (float)numItems) / 2.0f, (360.0f / (float)_segment.length()));
            float fadeDegrees = max((360.0f / (float)numItems) / 2.0f, (360.0f / (float)_segment.length())) * 1.5f;

            float angle = (float)segIdx * 360.0f / (float)_segment.length();

            float angleDiff = fabsf(angle - targetAngle);

            if (angleDiff > 180)
            {
                angleDiff = 360 - angleDiff;
            }

            if (angleDiff > fadeDegrees)
            {
                return 0;
            }

            // float t = angleDiff / fadeDegrees;
            // float returnVal = (-1.0f * (t * t)) + 1.0f;
            float t = angleDiff / fadeDegrees;
            float returnVal = cosf(t * M_PI * 0.5f);  // cos(0) = 1, cos(90°) = 0

            ESP_LOGV(TAG, "segIdx: %d currItem: %d numItems: %d targetAngle: %f LEDangle: %f angleDiff: %f returnVal: %f fadeDegrees: %f",
                     segIdx, currItem, numItems, targetAngle, angle, angleDiff, returnVal, fadeDegrees);

            return returnVal;
        }
    };
}
