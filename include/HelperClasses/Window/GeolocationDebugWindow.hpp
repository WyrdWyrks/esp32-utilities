#pragma once

#include "Window.hpp"
#include "States/GeolocationDebugState.hpp"
#include "DisplayUtilities.hpp"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // GeolocationDebugWindow
    // -------------------------------------------------------------------------
    // Cycles through every registered geolocation source, showing its
    // moniker, status, and an enable/disable toggle.
    // Refreshes while active; pauses refresh when covered by another
    // window (handled automatically through the queue-timeout mechanism).
    //
    // Usage:
    //   Utilities::pushWindow(std::make_shared<GeolocationDebugWindow>());

    class GeolocationDebugWindow : public Window
    {
    public:
        GeolocationDebugWindow()
        {
            _geolocationDebugState = std::make_shared<GeolocationDebugState>();

            registerInput(InputID::BUTTON_3, "Back");
            addInputCommand(InputID::BUTTON_3,
                [](const InputContext &) { Utilities::popWindow(); });

            setInitialState(_geolocationDebugState);
        }

    private:
        std::shared_ptr<GeolocationDebugState> _geolocationDebugState;
    };

} // namespace DisplayModule
