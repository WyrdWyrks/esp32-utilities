#pragma once

#include "LedPatternInterface.hpp"
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace UxModule
{
    class IlluminateButton : public LedPatternInterface
    {
    public:
        // inputIds are sorted ascending and auto-mapped to LEDs in the segment:
        // smallest inputId -> segment[0], next -> segment[1], etc.
        IlluminateButton(LedSegment segment, std::vector<uint8_t> inputIds)
            : LedPatternInterface(std::move(segment))
        {
            std::vector<uint8_t> sorted = inputIds;
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < sorted.size(); i++)
            {
                _inputIdSegIdx[sorted[i]] = i;
                _inputIdState[sorted[i]] = false;
            }

            animationTicks = 15;
        }

        void configurePattern(JsonDocument &config)
        {
            if (!config["inputStates"].isNull())
            {
                for (auto kvp : config["inputStates"].as<JsonArray>())
                {
                    if (!kvp["input"].isNull() && !kvp["state"].isNull())
                    {
                        _inputIdState[kvp["input"]] = kvp["state"];
                    }
                }
            }
        }

        bool iterateFrame()
        {
            ESP_LOGV(TAG, "Illuminate_Button::iterateFrame");
            for (auto& kvp : _inputIdSegIdx)
            {
                if (_inputIdState[kvp.first])
                {
                    _segment[kvp.second] = ThemeColor();
                }
                else
                {
                    _segment[kvp.second] = CRGB(0, 0, 0);
                }
            }

            return true;
        }

        void clearPattern()
        {
            LedPatternInterface::clearPattern();
            for (auto& kvp : _inputIdState)
            {
                kvp.second = false;
            }
        }

        void SetRegisteredPatternID(int patternID) { _RegisteredPatternId() = patternID; }
        static int RegisteredPatternID() { return _RegisteredPatternId(); }

    protected:
        static int &_RegisteredPatternId()
        {
            static int id = -1;
            return id;
        }

        std::unordered_map<uint8_t, size_t> _inputIdSegIdx;
        std::unordered_map<uint8_t, bool> _inputIdState;

        const char *TAG = "Illuminate_Button";
    };
}
