#pragma once

#include <FastLED.h>
#include "LedPatternInterface.hpp"
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace UxModule
{
    // Called when an input with an LED is pressed. LED turns on and fades away over the course of the animation length
    class ButtonFlash : public LedPatternInterface
    {
    public:
        ButtonFlash(LedSegment segment, std::vector<uint8_t> inputIds)
            : LedPatternInterface(std::move(segment))
        {
            for (size_t i = 0; i < inputIds.size(); i++)
            {
                _inputIdSegIdx[inputIds[i]] = i;
            }

            animationTicks = 15;
        }

        void configurePattern(JsonDocument &config)
        {
            if (!config["inputID"].isNull())
            {
                inputID = config["inputID"];
            }

            clearPattern();
        }

        bool iterateFrame()
        {
            if (startTime == 0)
            {
                startTime = xTaskGetTickCount();
            }

            auto it = _inputIdSegIdx.find(inputID);
            if (it == _inputIdSegIdx.end())
            {
                resetPattern();
                return true;
            }

            size_t segIdx = it->second;
            float brightness = 1.0 - (float)(xTaskGetTickCount() - startTime) / (float)animationMS;
            _segment[segIdx] = CRGB(ThemeColor().r * brightness, ThemeColor().g * brightness, ThemeColor().b * brightness);

            currTick++;

            if ((xTaskGetTickCount() - startTime) >= animationMS)
            {
                _segment[segIdx] = CRGB(0, 0, 0);
                resetPattern();
                return true;
            }
            else
            {
                return false;
            }
        }

        // Only claims its LEDs mid-flash; once the fade finishes (resetPattern
        // zeroes startTime) whatever sits below it shows through again.
        bool isActive() const override { return startTime != 0; }

        void SetRegisteredPatternID(int patternID) { _RegisteredPatternId() = patternID; }
        static int RegisteredPatternID() { return _RegisteredPatternId(); }

    protected:
        static int &_RegisteredPatternId()
        {
            static int id = -1;
            return id;
        }

        std::unordered_map<uint8_t, size_t> _inputIdSegIdx;
        uint8_t inputID;

        const char *TAG = "Button_Flash";
    };
}
