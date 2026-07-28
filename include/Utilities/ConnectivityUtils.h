#pragma once

#include <queue>
#include <memory>
#include <Arduino.h>
#include "SystemUtilities.hpp"
// #include "WiFiManager.h"
// #include "AlooWifiManager.h"

namespace ConnectivityModule
{
    
    static const char *TAG = "ConnectivityModule";

    enum WiFiProvisioningMode
    {
        WIFI_PROV_MODE_NONE = 0,
        WIFI_PROV_MODE_TEMP_AP = 2
    };

    class Utilities
    {
    public:
        // static WiFiManager &ProvisioningWiFiManager()
        // {
        //     // static WiFiManager wifiManager(WiFiProvisiningApSSID().c_str(), WiFiProvisiningApPassword().c_str());
        //     // return wifiManager;
        // }

        static WiFiProvisioningMode &ProvisioningMode()
        {
            static WiFiProvisioningMode mode = WIFI_PROV_MODE_NONE;
            return mode;
        }

        static std::string WiFiProvisiningApSSID()
        {
            auto ssid = System_Utils::DeviceName;;
            std::replace(ssid.begin(), ssid.end(), ' ', '_');
            return ssid;
        }

        // TODO: make this properly static and assigned at random
        static std::string WiFiProvisiningApPassword()
        {
            std::string deviceStr = std::to_string(System_Utils::DeviceID % 10000);
            return "setup" + std::string(4 - deviceStr.length(), '0') + deviceStr;
        }

        static void InitializeWiFiProvisioning()
        {
            if (ProvisioningMode() == WIFI_PROV_MODE_TEMP_AP)
            {
                // ProvisioningWiFiManager().setConfigPortalBlocking(false);
                // ProvisioningWiFiManager().autoConnect(WiFiProvisiningApSSID().c_str(), WiFiProvisiningApPassword().c_str());
                // ProvisioningWiFiManager().begin(true);
            }
        }

        static void DeinitializeWiFiProvisioning()
        {
            if (ProvisioningMode() == WIFI_PROV_MODE_TEMP_AP)
            {
                // ProvisioningWiFiManager().
                // WiFiManager().stopConfigPortal();
            }
        }

        static void ProcessSettings(JsonDocument &doc)
        {
            if (!doc["WiFi Provisioning"].isNull())
            {
                auto mode = doc["WiFi Provisioning"].as<int>();
                if (mode >= WIFI_PROV_MODE_NONE && mode <= WIFI_PROV_MODE_TEMP_AP)
                {
                    ProvisioningMode() = static_cast<WiFiProvisioningMode>(mode);
                }
                else
                {
                    ProvisioningMode() = WIFI_PROV_MODE_NONE;
                }
            }
        }
    };
}