#pragma once

#include <cstdio>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "NavigationUtils.h"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // GeolocationDebugState
    // -------------------------------------------------------------------------
    // Cycles through every registered GeolocationInterface source
    // (NavigationUtils::LocationSources()) and shows its moniker plus current
    // status: coordinates, "No Fix", "Polling...", or "Disabled".
    //
    // Never calls a source directly — it only reads each source's cached
    // GeolocationResult (GeolocationInterface::GetLastResult), so a slow
    // source (e.g. WiFi geolocation) can never block this screen. While open,
    // it puts the background poller into verbose mode (NavigationUtils::
    // SetVerbosePolling) so every enabled source gets polled each cycle
    // instead of stopping at the first success. BUTTON_1 requests an
    // immediate poll of just the currently-shown source
    // (NavigationUtils::RequestSourceRefresh) — still serviced by the single
    // background poll task, never called directly from here.
    //
    // Refreshes at 2 Hz via refreshIntervalMs() so the display picks up
    // whatever the background poller has published.
    //
    // Wiring example (Window):
    //   registerInput(InputID::BUTTON_3, "Back");
    //   addInputCommand(InputID::BUTTON_3, [](auto &) { Utilities::popWindow(); });

    class GeolocationDebugState : public WindowState
    {
    public:
        static constexpr uint32_t REFRESH_RATE_MS = 500;

        GeolocationDebugState()
        {
            bindInput(InputID::BUTTON_3, "Back");
            refreshIntervalMs = REFRESH_RATE_MS;

            bindInput(InputID::BUTTON_1, "Refresh", [this](const InputContext &) {
                auto &sources = NavigationUtils::LocationSources();
                if (_index >= sources.size()) return;
                NavigationUtils::RequestSourceRefresh(sources[_index]->GetMoniker());
            });

            bindInput(InputID::BUTTON_4, "Disable", [this](const InputContext &) {
                auto &sources = NavigationUtils::LocationSources();
                if (_index >= sources.size()) return;
                auto *src = sources[_index];
                NavigationUtils::SetSourceEnabled(src, !src->enabled);
                _rebuildDrawCommands();
            });

            bindInput(InputID::ENC_UP, "^", [this](const InputContext &) {
                auto &sources = NavigationUtils::LocationSources();
                if (sources.size() <= 1) return;
                _index = (_index == 0) ? sources.size() - 1 : _index - 1;
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_DOWN, "v", [this](const InputContext &) {
                auto &sources = NavigationUtils::LocationSources();
                if (sources.size() <= 1) return;
                _index = (_index + 1) % sources.size();
                _rebuildDrawCommands();
            });
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &) override
        {
            _index = 0;
            NavigationUtils::SetVerbosePolling(true);
            _rebuildDrawCommands();
        }

        void onExit() override
        {
            NavigationUtils::SetVerbosePolling(false);
            WindowState::onExit();
        }

        // ------------------------------------------------------------------
        // Tick — called each refresh cycle to pick up newly-published results
        // ------------------------------------------------------------------

        void onTick() override
        {
            _rebuildDrawCommands();
        }

    private:
        size_t _index = 0;

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            auto &sources = NavigationUtils::LocationSources();

            bindInput(InputID::ENC_UP, sources.size() <= 1 ? "" : "^");
            bindInput(InputID::ENC_DOWN, sources.size() <= 1 ? "" : "v");

            if (sources.empty())
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "No Sources",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 2 }
                ));
                return;
            }

            if (_index >= sources.size()) _index = 0;
            auto *src = sources[_index];

            uint8_t displayLine = 2;
            char headerBuf[32];
            snprintf(headerBuf, sizeof(headerBuf), "%s (%u/%u)", src->GetMoniker(),
                     static_cast<unsigned>(_index + 1), static_cast<unsigned>(sources.size()));
            addDrawCommand(std::make_shared<TextDrawCommand>(
                std::string(headerBuf),
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
            ));

            bindInput(InputID::BUTTON_4, src->enabled ? "Disable" : "Enable");

            if (!src->enabled)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Disabled",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
                return;
            }

            auto result = src->GetLastResult();

            if (!result.everPublished)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Polling...",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
                return;
            }

            if (result.hasFix)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _formatCoord("Lat", result.lat),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _formatCoord("Lon", result.lon),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
            }
            else
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "No Fix",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
                ));
            }

            addDrawCommand(std::make_shared<TextDrawCommand>(
                _formatAge(result.AgeMs()),
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, displayLine++ }
            ));
        }

        static std::string _formatCoord(const char *label, double value)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s: %.8f", label, value);
            return std::string(buf);
        }

        static std::string _formatAge(uint32_t ageMs)
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "Age: %lus", static_cast<unsigned long>(ageMs / 1000));
            return std::string(buf);
        }
    };

} // namespace DisplayModule
