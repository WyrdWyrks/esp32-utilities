#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "SystemUtilities.hpp"
#include "VersionUtils.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // DiagnosticsState
    // -------------------------------------------------------------------------
    // Displays the running firmware version plus live system diagnostics: free
    // heap (with unit scaling), heap fragmentation, and FreeRTOS task stack
    // high-water mark.
    //
    // Anything hardware-specific (PMIC rails, radio stats) comes from the
    // application layer via System_Utils::registerDiagnosticsProvider(); those
    // lines are appended below the built-in ones. See BootstrapMicrocontroller
    // for the V3 PMIC provider.
    //
    // Lines start at FIRST_LINE (2) because the WindowLayer input-label
    // factories own the top and bottom rows of the panel.
    //
    // Refreshes at 500 ms via refreshIntervalMs().
    //
    // Wiring example (Window):
    //   registerInput(InputID::BUTTON_3, "Back");
    //   addInputCommand(InputID::BUTTON_3, [](auto &) { Utilities::popWindow(); });

    class DiagnosticsState : public WindowState
    {
    public:
        static constexpr uint32_t REFRESH_RATE_MS = 500;

        // 1-based display line the first diagnostic lands on. Line 1 is taken by
        // the input labels drawn at the top of the panel.
        static constexpr uint8_t FIRST_LINE = 2;

        DiagnosticsState()
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
        // Tick — update diagnostics each cycle
        // ------------------------------------------------------------------

        void onTick() override
        {
            _rebuildDrawCommands();
        }

    private:
        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            std::vector<std::string> lines;

            char buf[32];

            snprintf(buf, sizeof(buf), "FW: %s", FIRMWARE_VERSION_STRING);
            lines.emplace_back(buf);

            // Heap free with unit scaling
            uint32_t heap = ESP.getFreeHeap();
            const char *units = "b";
            if (heap > 1024) { heap >>= 10; units = "Kb"; }
            if (heap > 1024) { heap >>= 10; units = "Mb"; }

            snprintf(buf, sizeof(buf), "Heap: %lu%s",
                     static_cast<unsigned long>(heap), units);
            lines.emplace_back(buf);

            // Fragmentation: 1 - (maxContiguous / freeHeap). 0% means all free
            // memory is one contiguous block; ~100% means it is heavily split up.
            if (ESP.getFreeHeap() > 0)
            {
                float frag = 1.0f - static_cast<float>(ESP.getMaxAllocHeap())
                                    / static_cast<float>(ESP.getFreeHeap());
                snprintf(buf, sizeof(buf), "Frag: %.0f%%",
                         static_cast<double>(frag * 100.0f));
            }
            else
            {
                snprintf(buf, sizeof(buf), "Frag: N/A");
            }
            lines.emplace_back(buf);

            // Stack high-water mark of the calling task (display task)
            snprintf(buf, sizeof(buf), "Stack Min: %lu",
                     static_cast<unsigned long>(uxTaskGetStackHighWaterMark(NULL)));
            lines.emplace_back(buf);

            // Hardware-specific lines supplied by the application layer
            auto extraLines = System_Utils::collectDiagnostics();
            lines.insert(lines.end(),
                         std::make_move_iterator(extraLines.begin()),
                         std::make_move_iterator(extraLines.end()));

            uint8_t displayLine = FIRST_LINE;
            for (auto &line : lines)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    std::move(line),
                    TextFormat{ TextAlignH::LEFT, TextAlignV::LINE, displayLine++ }
                ));
            }
        }
    };

} // namespace DisplayModule
