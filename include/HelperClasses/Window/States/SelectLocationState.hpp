#pragma once

#include <vector>
#include <string>
#include <memory>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "NavigationUtils.h"
#include "LedUtilities.hpp"
#include "ScrollWheel.hpp"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // SelectLocationState
    // -------------------------------------------------------------------------
    // Presents a scrollable list of SavedLocations for the user to choose one.
    //
    // Payload in:
    //   {
    //     "Locations": [
    //       { "Name": <str>, "Lat": <double>, "Lng": <double> }, ...
    //     ]
    //   }
    //
    // Payload out (on BUTTON_4 — Select):
    //   { "Name": <str>, "Lat": <double>, "Lng": <double> }
    //
    // No payload on BUTTON_3 (Back / cancel).
    //
    // Input layout (wired by owning Window):
    //   ENC_UP / ENC_DOWN — scroll
    //   BUTTON_4          — "Select" (pop state with selected location)
    //   BUTTON_3          — "Back"   (pop state without payload)
    //
    // The owning Window calls buildSelectPayload() to construct the return
    // payload before calling popState().

    class SelectLocationState : public WindowState
    {
    public:
        SelectLocationState()
        {
            bindInput(InputID::ENC_UP, _locations.size() <= 1 ? "" : "^", [this](const InputContext &) {
                if (_locations.size() <= 1) return;
                if (_it == _locations.begin())
                    _it = _locations.end() - 1;
                else
                    --_it;
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_DOWN, _locations.size() <= 1 ? "" : "v", [this](const InputContext &) {
                if (_locations.size() <= 1) return;
                ++_it;
                if (_it == _locations.end())
                    _it = _locations.begin();
                _rebuildDrawCommands();
            });
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &data) override
        {
            _locations.clear();

            if (data.payload)
            {
                auto &doc = *data.payload;
                if (!doc["Locations"].isNull()
                    && doc["Locations"].is<ArduinoJson::JsonArray>())
                {
                    for (auto loc : doc["Locations"].as<ArduinoJson::JsonArrayConst>())
                    {
                        SavedLocation sl;
                        sl.Name      = loc["Name"].as<std::string>();
                        sl.Latitude  = loc["Lat"].as<double>();
                        sl.Longitude = loc["Lng"].as<double>();
                        ESP_LOGV(TAG, "Loaded location: %s (Lat: %f, Lng: %f)", sl.Name.c_str(), sl.Latitude, sl.Longitude);
                        _locations.push_back(sl);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "SelectLocationState expected payload with 'Locations' array");
                }
            }

            _it            = _locations.begin();
            _scrollWheelID = UxModule::ScrollWheel::RegisteredPatternID();
            UxModule::LedUtilities::enablePattern(_scrollWheelID);

            _rebuildDrawCommands();
        }

        void onExit() override
        {
            // disablePattern() clears the pattern's own segment via the
            // LedPatternInterface (pattern->clearPattern() -> _segment.clear()),
            // so the state never needs to know which physical LEDs it used.
            UxModule::LedUtilities::disablePattern(_scrollWheelID);
            WindowState::onExit();
        }

        // ------------------------------------------------------------------
        // Payload builder for owning Window (BUTTON_4 handler)
        // ------------------------------------------------------------------

        void buildSelectPayload(std::shared_ptr<ArduinoJson::JsonDocument> doc) const
        {
            if (_locations.empty() || _it == _locations.end())
                return;

            doc->operator[]("Name") = _it->Name;
            doc->operator[]("Lat")  = _it->Latitude;
            doc->operator[]("Lng")  = _it->Longitude;
        }

        bool hasLocations() const { return !_locations.empty(); }

    private:
        std::vector<SavedLocation>          _locations;
        std::vector<SavedLocation>::iterator _it;
        int _scrollWheelID = -1;

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            bindInput(InputID::ENC_UP, _locations.size() <= 1 ? "" : "^");
            bindInput(InputID::ENC_DOWN, _locations.size() <= 1 ? "" : "v");

            if (!_locations.empty() && _it != _locations.end())
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Select a location",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 2 }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _it->Name,
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 3 }
                ));

                if (_scrollWheelID >= 0)
                {
                    ArduinoJson::JsonDocument cfg;
                    cfg["numItems"] = _locations.size();
                    cfg["currItem"] = std::distance(_locations.begin(), _it);
                    UxModule::LedUtilities::configurePattern(_scrollWheelID, cfg);
                    UxModule::LedUtilities::iteratePattern(_scrollWheelID);
                }
            }
            else
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "No locations",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::CENTER }
                ));
            }
        }
    };

} // namespace DisplayModule
