#pragma once

#include <cstdio>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "NavigationUtils.h"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // GpsState
    // -------------------------------------------------------------------------
    // Displays current latitude, longitude, and which registered
    // GeolocationInterface source produced the fix (e.g. "GpsSource",
    // "StaticLocation") — useful for debugging.
    // Refreshes at 1 Hz via refreshIntervalMs().
    //
    // Shows "GPS Not Connected" when no fix is available.
    //
    // Wiring example (Window):
    //   registerInput(InputID::BUTTON_3, "Back");
    //   addInputCommand(InputID::BUTTON_3, [](auto &) { Utilities::popWindow(); });

    class GpsState : public WindowState
    {
    public:
        static constexpr uint32_t REFRESH_RATE_MS = 1000;

        GpsState()
        {
            bindInput(InputID::BUTTON_3, "Back");
            refreshIntervalMs = REFRESH_RATE_MS;
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &) override
        {
            _rebuildDrawCommands();
        }

        // ------------------------------------------------------------------
        // Tick — called each refresh cycle to update GPS readings
        // ------------------------------------------------------------------

        void onTick() override
        {
            _rebuildDrawCommands();
        }

    private:
        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            double myLat, myLon;
            std::string moniker;
            uint8_t displayLine = 1;

            if (NavigationUtils::GetCurrentLocation(myLat, myLon, moniker))
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _formatCoord("Lat", myLat),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _formatCoord("Lon", myLon),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Source: " + moniker,
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
            }
            else
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "GPS Not Connected",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 2 }
                ));
            }
        }

        static std::string _formatCoord(const char *label, double value)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s: %.8f", label, value);
            return std::string(buf);
        }
    };

} // namespace DisplayModule
