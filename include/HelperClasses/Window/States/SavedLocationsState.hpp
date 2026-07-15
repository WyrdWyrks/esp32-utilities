#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdio>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "NavigationUtils.h"
#include "LED_Utils.h"
#include "RingPoint.hpp"

namespace { constexpr size_t MAX_LOCATION_NAME_LENGTH = 21; }

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // SavedLocationsState
    // -------------------------------------------------------------------------
    // Scrollable list of saved GPS locations with edit and delete actions, plus
    // *live location tracking*: the RingPoint LED points toward the currently
    // selected location and the distance readout updates every tick, so scrolling
    // the list retargets the compass immediately (mirrors ViewMessageState, which
    // does the same for an incoming PingMessage instead of a saved coordinate).
    //
    // Input layout (wired by owning Window):
    //   ENC_UP / ENC_DOWN — scroll list (retargets tracking)
    //   BUTTON_3          — "Back" (pop window)
    //   BUTTON_4          — "Edit"   — open EditStringState with name
    //   BUTTON_1          — "Delete" — remove selected location
    //
    // Return from EditStringState:
    //   payload["return"] = new name string → updates selected location's name
    //
    // Getters used by the owning Window to build state-transition payloads:
    //   buildEditPayload() — current name for EditString prefill
    //
    // LED: RingPoint pattern, enabled on enter, disabled on exit; reconfigured on
    // every scroll and every tick to track the selected location's bearing.

    class SavedLocationsState : public WindowState
    {
    public:
        static constexpr uint32_t REFRESH_RATE_MS = 100;

        SavedLocationsState()
        {
            refreshIntervalMs = REFRESH_RATE_MS;

            bindInput(InputID::BUTTON_1, "Delete", [this](const InputContext &) {
                size_t count = NavigationUtils::GetSavedLocationsSize();
                if (count == 0) return;
                NavigationUtils::RemoveSavedLocation(_selectedIt);
                if (_selectedIt == NavigationUtils::GetSavedLocationsEnd()
                    && NavigationUtils::GetSavedLocationsSize() > 0)
                {
                    --_selectedIt;
                }
                _configureLed();
                _rebuildDrawCommands();
            });
            bindInput(InputID::BUTTON_3, "Back");
            bindInput(InputID::BUTTON_4, "Edit");
            bindInput(InputID::ENC_UP, "", [this](const InputContext &) {
                size_t count = NavigationUtils::GetSavedLocationsSize();
                if (count == 0) return;
                if (_selectedIt == NavigationUtils::GetSavedLocationsBegin())
                    _selectedIt = NavigationUtils::GetSavedLocationsEnd();
                --_selectedIt;
                _configureLed();
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_DOWN, "", [this](const InputContext &) {
                size_t count = NavigationUtils::GetSavedLocationsSize();
                if (count == 0) return;
                ++_selectedIt;
                if (_selectedIt == NavigationUtils::GetSavedLocationsEnd())
                    _selectedIt = NavigationUtils::GetSavedLocationsBegin();
                _configureLed();
                _rebuildDrawCommands();
            });
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &data) override
        {
            // Returned from edit state? Apply the new name.
            if (data.payload && !(*data.payload)["return"].isNull())
            {
                std::string newName = (*data.payload)["return"].as<std::string>();
                if (_selectedIt != NavigationUtils::GetSavedLocationsEnd()
                    && NavigationUtils::GetSavedLocationsSize() > 0)
                {
                    SavedLocation updated = *_selectedIt;
                    updated.Name = newName;
                    NavigationUtils::UpdateSavedLocation(_selectedIt, updated);
                }
            }
            else
            {
                // Fresh entry — reset iterator
                _selectedIt = NavigationUtils::GetSavedLocationsBegin();
            }

            _ringPointID = RingPoint::RegisteredPatternID();
            LED_Utils::enablePattern(_ringPointID);

            _configureLed();
            _rebuildDrawCommands();
        }

        void onExit() override
        {
            LED_Utils::disablePattern(_ringPointID);
            WindowState::onExit();
        }

        void onPause() override
        {
            if (_ringPointID >= 0)
                LED_Utils::disablePattern(_ringPointID);
        }

        void onResume() override
        {
            if (_ringPointID >= 0)
                LED_Utils::enablePattern(_ringPointID);
        }

        // ------------------------------------------------------------------
        // Tick — retarget the compass and refresh the distance readout
        // ------------------------------------------------------------------

        void onTick() override
        {
            _configureLed();
            _rebuildDrawCommands();
        }

        // ------------------------------------------------------------------
        // Payload builders — called by the owning Window's input commands
        // ------------------------------------------------------------------

        // For BUTTON_4 (Edit) — sends current name to EditStringState
        std::shared_ptr<ArduinoJson::JsonDocument> buildEditPayload() const
        {
            auto doc = std::make_shared<ArduinoJson::JsonDocument>();
            if (NavigationUtils::GetSavedLocationsSize() > 0
                && _selectedIt != NavigationUtils::GetSavedLocationsEnd())
            {
                (*doc)["cfgVal"] = _selectedIt->Name;
                (*doc)["maxLen"] = static_cast<int>(MAX_LOCATION_NAME_LENGTH);
            }
            return doc;
        }

        bool hasLocations() const
        {
            return NavigationUtils::GetSavedLocationsSize() > 0;
        }

    private:
        std::vector<SavedLocation>::iterator _selectedIt;
        int _ringPointID = -1;

        // Points the RingPoint LED at the selected location, with the point's
        // sharpness (fadeDegrees) tightening as the target gets closer. Mirrors
        // ViewMessageState::_configureLed().
        void _configureLed()
        {
            if (_ringPointID < 0
                || NavigationUtils::GetSavedLocationsSize() == 0
                || _selectedIt == NavigationUtils::GetSavedLocationsEnd())
            {
                return;
            }

            double distance = NavigationUtils::GetDistanceTo(
                _selectedIt->Latitude, _selectedIt->Longitude);

            // No GPS fix (GetDistanceTo returns < 0): we can't know which way to
            // point, so clear the ring rather than show a bogus heading. The list
            // stays scrollable and the readout shows "No Location Data"; the ring
            // relights on the next tick once a fix returns.
            if (distance < 0)
            {
                LED_Utils::clearPattern(_ringPointID);
                return;
            }

            const double ledFxMin = 20;
            const double ledFxMax = 500;
            if (distance > ledFxMax)      { distance = ledFxMax; }
            else if (distance < ledFxMin) { distance = ledFxMin; }

            float directionDegrees = _getLocationBearing();
            float fadeDegrees = -0.075f * distance + 61.5f;

            ArduinoJson::JsonDocument cfg;
            cfg["fadeDegrees"]      = fadeDegrees;
            cfg["directionDegrees"] = directionDegrees;

            LED_Utils::configurePattern(_ringPointID, cfg);
            LED_Utils::iteratePattern(_ringPointID);
        }

        // Bearing to the selected location relative to the device's heading, so
        // the LED ring points the right way as the user turns.
        float _getLocationBearing()
        {
            if (_selectedIt == NavigationUtils::GetSavedLocationsEnd()) { return 0.0f; }

            double heading = NavigationUtils::GetHeadingTo(
                _selectedIt->Latitude, _selectedIt->Longitude);
            return NavigationUtils::GetBearing(heading);
        }

        // Live distance to the selected location, or a placeholder when we have
        // no current fix.
        std::string _distanceText() const
        {
            double myLat = 0.0, myLon = 0.0;
            if (!NavigationUtils::GetCurrentLocation(myLat, myLon))
            {
                return "No Location Data";
            }

            double distance = NavigationUtils::GetDistance(
                myLat, myLon, _selectedIt->Latitude, _selectedIt->Longitude);

            char distBuf[24];
            snprintf(distBuf, sizeof(distBuf), "Dist: %.1f m", distance);
            return std::string(distBuf);
        }

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            size_t count = NavigationUtils::GetSavedLocationsSize();
            if (count == 0 || _selectedIt == NavigationUtils::GetSavedLocationsEnd())
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "No Saved Locations",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::CENTER }
                ));
                return;
            }

            auto midScreen = Utilities::selectBottomTextLine() >> 1; // roughly center the name and distance readout

            // Selected location name (prominent, centered).
            addDrawCommand(std::make_shared<TextDrawCommand>(
                _selectedIt->Name,
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, static_cast<uint8_t>(midScreen++) }
            ));

            // Live distance readout pinned to the bottom line.
            addDrawCommand(std::make_shared<TextDrawCommand>(
                _distanceText(),
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, static_cast<uint8_t>(midScreen) }
            ));
        }
    };

} // namespace DisplayModule
