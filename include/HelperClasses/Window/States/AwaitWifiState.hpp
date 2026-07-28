#pragma once

#include <string>
#include "WindowState.hpp"
#include "TextDrawCommand.hpp"
#include "RadioUtils.h"
#include "ConnectivityUtils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // AwaitWifiState
    // -------------------------------------------------------------------------
    // Manages WiFi connection flow:
    //   1. On enter: tries saved credentials; if unavailable, starts WiFi
    //      provisioning (temporary AP or ESP-NOW SmartConfig fallback).
    //   2. Refreshes at 500 ms — each tick updates the display with current
    //      connection status and, if the timeout elapses, falls over to
    //      provisioning mode.
    //
    // The owning window (OtaUpdateWindow, WiFiRpcWindow) switches away from
    // this state once ConnectivityModule::RadioUtils::IsWiFiActive() returns
    // true.  That check is done in the Window's onTick() or as an input
    // command callback on the rendered event.
    //
    // BUTTON_3 (Back) should be wired by the owning Window.

    class AwaitWifiState : public WindowState
    {
    public:
        static constexpr uint32_t REFRESH_RATE_MS    = 500;
        static constexpr TickType_t WIFI_TIMEOUT_MS  = 10000;

        AwaitWifiState()
        {
            bindInput(InputID::BUTTON_3, "Cancel");
            refreshIntervalMs = REFRESH_RATE_MS;
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &) override
        {
            _connectBegin       = xTaskGetTickCount();
            _skipTimeout        = true;
            _awaitProvisioning  = false;

            // Try saved AP credentials first
            if (ConnectivityModule::RadioUtils::ConnectToAccessPoint())
            {
                _skipTimeout       = false;
                _awaitProvisioning = false;
            }
            else
            {
                bool credentialSaved = !(WiFi.SSID() == "");
                if (credentialSaved)
                {
                    auto numNetworks = WiFi.scanNetworks();
                    for (int i = 0; i < numNetworks; ++i)
                    {
                        if (WiFi.SSID() == WiFi.SSID(i))
                        {
                            _skipTimeout = false;
                            WiFi.begin();
                            break;
                        }
                    }
                }

                if (_skipTimeout)
                {
                    ConnectivityModule::Utilities::InitializeWiFiProvisioning();
                    _awaitProvisioning = true;
                }
            }

            _rebuildDrawCommands();
        }

        void onExit() override
        {
            if (ConnectivityModule::RadioUtils::RadioState()
                != ConnectivityModule::RADIO_STATE_STA)
            {
                ConnectivityModule::Utilities::DeinitializeWiFiProvisioning();
            }
            WindowState::onExit();
        }

        // ------------------------------------------------------------------
        // Tick — advance timeout, update display
        // ------------------------------------------------------------------

        void onTick() override
        {
            if (!_awaitProvisioning
                && (_skipTimeout
                    || xTaskGetTickCount() > _connectBegin + pdMS_TO_TICKS(WIFI_TIMEOUT_MS)))
            {
                ConnectivityModule::Utilities::InitializeWiFiProvisioning();
                _awaitProvisioning = true;
            }

            _rebuildDrawCommands();
        }

    private:
        TickType_t _connectBegin      = 0;
        bool       _skipTimeout       = true;
        bool       _awaitProvisioning = false;

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            if (!_awaitProvisioning)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Connecting to WiFi...",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::CENTER }
                ));
                return;
            }

            auto mode = ConnectivityModule::Utilities::ProvisioningMode();

            if (mode == ConnectivityModule::WIFI_PROV_MODE_TEMP_AP)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Configure WiFi",
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 1 }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "SSID: " + ConnectivityModule::Utilities::WiFiProvisiningApSSID(),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 2 }
                ));
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    "Pwd: " + ConnectivityModule::Utilities::WiFiProvisiningApPassword(),
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 3 }
                ));
            }
        }
    };

} // namespace DisplayModule
