#pragma once

#include "SystemUtilities.hpp"
#include "ConnectivityUtils.h"
#include "WiFi.h"
#include <esp_now.h>
#include "esp_smartconfig.h"
#include "esp_wifi.h"

namespace ConnectivityModule
{

    enum WiFiRadioState
    {
        RADIO_STATE_OFF = 0,
        RADIO_STATE_STA = 1,
        RADIO_STATE_AP = 2,
        RADIO_STATE_ESP_NOW = 3,
        RADIO_STATE_BT = 4
    };

    class RadioUtils
    {
    public:
        static WiFiRadioState &RadioState()
        {
            static WiFiRadioState radioState = RADIO_STATE_OFF;
            return radioState;
        }

        static void EnableRadio()
        {
            WiFi.setSleep(false);
        }

        // NOTE: This does NOT power the radio down. It only enables WiFi
        // modem-sleep (light doze between DTIM beacons) and marks the tracked
        // state OFF; the PHY stays powered. For a real power-down that stops
        // the PHY, use ReleaseAfterScan() (scan borrowing) or
        // StopAccessPoint()/DeinitializeEspNow(), which call WiFi.mode(WIFI_OFF).
        static void DisableRadio()
        {
            WiFi.setSleep(true);
            RadioState() = RADIO_STATE_OFF;
        }

        // ------------------------------------------------------------------
        // Power-managed scan borrowing
        // ------------------------------------------------------------------
        // Keeps the radio fully powered down whenever nothing needs it. A
        // caller that needs WiFi only momentarily (e.g. a geolocation scan)
        // borrows it with TryAcquireForScan(), does its work, then calls
        // ReleaseAfterScan() to power the PHY back off. If a long-lived owner
        // already has WiFi up (RPC AP/STA session, provisioning, ESP-NOW), the
        // borrow is declined so the scan never disturbs an active connection.

        // Brings STA up and returns true if the radio was free; returns false
        // and changes nothing if WiFi is already in use elsewhere (the caller
        // should skip its scan this cycle).
        static bool TryAcquireForScan()
        {
            if (WiFi.getMode() != WIFI_MODE_NULL)
            {
                return false; // radio already owned (AP/STA/provisioning/ESP-NOW)
            }

            _ScanOwnsRadio() = true;
            WiFi.mode(WIFI_STA); // scanNetworks() does this too; explicit here
            RadioState() = RADIO_STATE_STA;
            return true;
        }

        // Powers the radio fully off, but only if a prior TryAcquireForScan()
        // is what brought it up. No-op otherwise, so it is safe to call on
        // every scan-exit path.
        static void ReleaseAfterScan()
        {
            if (!_ScanOwnsRadio())
            {
                return;
            }

            _ScanOwnsRadio() = false;
            WiFi.disconnect(true); // tear down the STA interface
            WiFi.mode(WIFI_OFF);   // real power-down (cf. DisableRadio modem-sleep)
            RadioState() = RADIO_STATE_OFF;
        }

        static bool IsRadioActive()
        {
            return !WiFi.getSleep();
        }

        static int RadioChannel()
        {
            return WiFi.channel();
        }

        static uint8_t &EspNowChanel()
        {
            static uint8_t channel = 1;
            return channel;
        }

        static void SetRadioChannel(uint8_t channel)
        {
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        static void SetRadioChannel()
        {
            SetRadioChannel(EspNowChanel());
        }

        static void InitializeEspNow()
        {
            WiFi.mode(WIFI_STA);
            WiFi.disconnect();
            EnableRadio();
            SetRadioChannel(EspNowChanel());

            if (esp_now_init() != ESP_OK)
            {
                DisableRadio();
                ESP_LOGE(TAG, "esp_now_init failed");
                return;
            }

            RadioState() = RADIO_STATE_ESP_NOW;
            // TODO: re-add saved peers as they get deleted when esp-now is disabled.
        }

        static void DeinitializeEspNow()
        {
            esp_now_deinit();
            DisableRadio();
            WiFi.disconnect();
            WiFi.mode(WIFI_OFF);
        }

        static void InitializeSmartConfig()
        {
            EnableRadio();
            WiFi.disconnect();
            WiFi.mode(WIFI_STA);
            WiFi.beginSmartConfig();
        }

        // STA and SmartConfig
        static bool CheckSmartConfig()
        {
            auto result = WiFi.smartConfigDone();
            if (result)
            {
                RadioState() = RADIO_STATE_STA;
                return true;
            }

            return false;
        }

        static void DeinitializeSmartConfig()
        {
            WiFi.stopSmartConfig();
        }

        static bool IsWiFiActive()
        {
            return WiFi.status() == WL_CONNECTED;
        }

        // Connects to AP using saved password from last SmartConfig connection
        static bool ConnectToAccessPoint()
        {
            EnableRadio();
            WiFi.disconnect();
            WiFi.mode(WIFI_STA);
            WiFi.begin();

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED)
            {
                if (millis() - start > 10000)  // 10 second timeout
                {
                    ESP_LOGW(TAG, "WiFi connection timed out");
                    return false;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
            }

            RadioState() = RADIO_STATE_STA;
            return true;
        }

        static bool ConnectToAccessPoint(std::string ssid, std::string password)
        {
            EnableRadio();
            WiFi.disconnect();
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), password.c_str());

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED)
            {
                if (millis() - start > 10000)  // 10 second timeout
                {
                    ESP_LOGW(TAG, "WiFi connection timed out");
                    return false;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
            }

            RadioState() = RADIO_STATE_STA;
            ESP_LOGI(TAG, "Connected to %s, IP: %s", ssid.c_str(), WiFi.localIP().toString().c_str());
            return true;
        }

        // AP
        static std::string &ApSSID()
        {
            static std::string _ApSSID = "ESP32-Utilities-AP";
            return _ApSSID;
        }

        static std::string &ApPassword()
        {
            static std::string _ApPassword = "esp-ap-password";
            return _ApPassword;
        }

        static std::string GetWiFiIpAddress()
        {
            if (RadioState() == RADIO_STATE_AP)
            {
                ESP_LOGI(TAG, "Returning AP IP address: %s", WiFi.softAPIP().toString().c_str());
                return WiFi.softAPIP().toString().c_str();
            }
            else if (RadioState() == RADIO_STATE_STA)
            {
                ESP_LOGI(TAG, "Returning STA IP address: %s", WiFi.localIP().toString().c_str());
                return WiFi.localIP().toString().c_str();
            }
            else
            {
                ESP_LOGI(TAG, "Radio state is %d, returning default IP address", RadioState());
                return "0.0.0.0";
            }
        }

        static bool StartAccessPoint()
        {
            WiFi.mode(WIFI_AP);
            auto result = WiFi.softAP(ApSSID().c_str(), ApPassword().c_str());
            if (result)
            {
                RadioState() = RADIO_STATE_AP;
            }
            return result;
        }

        static void StopAccessPoint()
        {
            WiFi.softAPdisconnect();
            DisableRadio();
            WiFi.mode(WIFI_OFF);
            RadioState() = RADIO_STATE_OFF;
        }

    private:
        // True while a geolocation scan (and only a scan) is holding the radio,
        // so ReleaseAfterScan() only powers down what TryAcquireForScan()
        // powered up.
        static bool &_ScanOwnsRadio()
        {
            static bool owns = false;
            return owns;
        }
    };
}