#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "SystemUtilities.hpp"
#include "VersionUtils.h"
#include "esp_system.h"
#include "esp_timer.h"
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

            // Why the last boot happened. Distinguishes a brownout (power rail
            // sagging under a WiFi/LoRa TX burst) from a panic or a watchdog,
            // which is the first question when a device reboots on its own.
            snprintf(buf, sizeof(buf), "Reset: %s", _ResetReasonLabel(esp_reset_reason()));
            lines.emplace_back(buf);

            // Time since that boot. Read against the reset reason: a device that
            // says Brownout with a short uptime every visit is resetting in a
            // loop; one that has been up for days just had a one-off.
            _FormatUptime(buf, sizeof(buf));
            lines.emplace_back(buf);

            // Heap free with unit scaling
            _FormatBytes(buf, sizeof(buf), "Heap: ", ESP.getFreeHeap());
            lines.emplace_back(buf);

            // Lowest the free heap has been since boot — a number that keeps
            // falling between visits is a leak or fragmentation on its way to
            // an allocation failure.
            _FormatBytes(buf, sizeof(buf), "Min Heap: ", ESP.getMinFreeHeap());
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

        // "<label><N><unit>" with the same b/Kb/Mb scaling the heap line has
        // always used.
        static void _FormatBytes(char *buf, size_t bufLen, const char *label, uint32_t bytes)
        {
            const char *units = "b";
            if (bytes > 1024) { bytes >>= 10; units = "Kb"; }
            if (bytes > 1024) { bytes >>= 10; units = "Mb"; }
            snprintf(buf, bufLen, "%s%lu%s", label, static_cast<unsigned long>(bytes), units);
        }

        // "Up: 1d 02:34:56" (days only once there are any). Uses the 64-bit
        // esp_timer clock rather than millis(), which wraps after 49 days.
        static void _FormatUptime(char *buf, size_t bufLen)
        {
            uint64_t secs = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
            unsigned days = static_cast<unsigned>(secs / 86400);
            unsigned h    = static_cast<unsigned>((secs / 3600) % 24);
            unsigned m    = static_cast<unsigned>((secs / 60) % 60);
            unsigned s    = static_cast<unsigned>(secs % 60);
            if (days > 0)
            {
                snprintf(buf, bufLen, "Up: %ud %02u:%02u:%02u", days, h, m, s);
            }
            else
            {
                snprintf(buf, bufLen, "Up: %02u:%02u:%02u", h, m, s);
            }
        }

        // Short labels so "Reset: " + label fits the 21-character line. Only
        // the reasons every supported IDF defines; anything newer reads Other.
        static const char *_ResetReasonLabel(esp_reset_reason_t reason)
        {
            switch (reason)
            {
                case ESP_RST_POWERON:   return "Power On";
                case ESP_RST_EXT:       return "External";
                case ESP_RST_SW:        return "Software";
                case ESP_RST_PANIC:     return "Panic";
                case ESP_RST_INT_WDT:   return "Int WDT";
                case ESP_RST_TASK_WDT:  return "Task WDT";
                case ESP_RST_WDT:       return "Other WDT";
                case ESP_RST_DEEPSLEEP: return "Deep Sleep";
                case ESP_RST_BROWNOUT:  return "Brownout";
                case ESP_RST_SDIO:      return "SDIO";
                case ESP_RST_UNKNOWN:   return "Unknown";
                default:                return "Other";
            }
        }
    };

} // namespace DisplayModule
