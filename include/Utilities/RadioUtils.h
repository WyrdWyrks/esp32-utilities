#pragma once

#include "SystemUtilities.hpp"
#include "ConnectivityUtils.h"
#include "WiFi.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace ConnectivityModule
{

    enum WiFiRadioState
    {
        RADIO_STATE_OFF = 0,
        RADIO_STATE_STA = 1,
        RADIO_STATE_AP = 2,
        RADIO_STATE_BT = 3
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
        // the PHY, use ReleaseAfterScan() (scan borrowing), ReleaseWiFi(), or
        // StopAccessPoint(), which call WiFi.mode(WIFI_OFF).
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
        // already has WiFi up (RPC AP/STA session, provisioning) or Bluetooth
        // holds the radio, the borrow is declined so the scan never disturbs
        // an active connection.

        // Brings STA up and returns true if the radio was free; returns false
        // and changes nothing if WiFi is already in use elsewhere (the caller
        // should skip its scan this cycle).
        static bool TryAcquireForScan()
        {
            RadioLock lock;

            // Only borrow the radio when it is genuinely idle: the state
            // machine says OFF *and* the WiFi driver is down. Any other owner
            // — an AP/STA session (non-null WiFi mode) or Bluetooth (which
            // leaves WiFi mode NULL, so the tracked state is the only signal)
            // — makes the scan defer and try again next cycle.
            if (RadioState() != RADIO_STATE_OFF || WiFi.getMode() != WIFI_MODE_NULL)
            {
                return false;
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
            RadioLock lock;

            // If a higher-priority owner (Bluetooth) preempted the scan while
            // it was running, it revoked this flag and already re-owns the
            // radio — so we must not power anything down or reset the state.
            if (!_ScanOwnsRadio())
            {
                return;
            }

            _ScanOwnsRadio() = false;
            WiFi.disconnect(true); // tear down the STA interface
            WiFi.mode(WIFI_OFF);   // real power-down (cf. DisableRadio modem-sleep)
            RadioState() = RADIO_STATE_OFF;
        }

        // ------------------------------------------------------------------
        // Bluetooth ownership
        // ------------------------------------------------------------------
        // The ESP32 shares one 2.4 GHz radio between WiFi and BLE, so the two
        // are mutually exclusive here: claiming the radio for Bluetooth powers
        // WiFi down and records RADIO_STATE_BT, which makes TryAcquireForScan()
        // defer. The NimBLE bring-up/teardown itself lives in
        // BluetoothUtilities; these methods only move radio ownership.

        // Claims the radio for Bluetooth. Waits (briefly) for an in-flight
        // geolocation scan to finish and release rather than yanking WiFi out
        // from under a blocking scan; scans are short, so this is normally
        // instant. Returns once BLE owns the radio.
        static void AcquireForBluetooth(uint32_t maxScanWaitMs = 5000)
        {
            _AcquireRadio(RADIO_STATE_BT, maxScanWaitMs);
            // BLE needs the shared radio to itself; make sure WiFi is fully
            // down. Safe to do outside the lock: the radio is already marked
            // RADIO_STATE_BT, so no scan will start underneath us.
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        }

        // Releases the radio held by Bluetooth back to OFF. Call after the
        // NimBLE stack has been torn down (BluetoothUtilities::deinitBluetooth).
        static void ReleaseBluetooth()
        {
            RadioLock lock;
            if (RadioState() == RADIO_STATE_BT)
            {
                RadioState() = RADIO_STATE_OFF;
            }
        }

        // ------------------------------------------------------------------
        // WiFi session ownership
        // ------------------------------------------------------------------
        // AP and STA sessions (RPC config, provisioning, foreground scans)
        // share the same arbiter as Bluetooth. AcquireForWiFi() waits out any
        // in-flight geolocation scan and records the ownership state so
        // background scans defer; the caller then performs its own WiFi
        // bring-up (WiFi.mode()/begin()/softAP()/scanNetworks()). ReleaseWiFi()
        // powers the interface back down and returns the radio to OFF.

        // Claims the radio for a WiFi session in the given mode (STA by
        // default, or AP). Does not itself change the WiFi mode — the caller
        // does that after this returns. Pair with ReleaseWiFi().
        static void AcquireForWiFi(WiFiRadioState mode = RADIO_STATE_STA,
                                   uint32_t maxScanWaitMs = 5000)
        {
            _AcquireRadio(mode, maxScanWaitMs);
        }

        // Powers WiFi (STA or AP) fully down and returns the radio to OFF so
        // geolocation and radio-off idle resume. No-op if WiFi isn't the
        // current owner.
        static void ReleaseWiFi()
        {
            RadioLock lock;
            if (RadioState() == RADIO_STATE_STA || RadioState() == RADIO_STATE_AP)
            {
                WiFi.disconnect(true);
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_OFF);
                RadioState() = RADIO_STATE_OFF;
            }
        }

        static bool IsRadioActive()
        {
            return !WiFi.getSleep();
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
            _AcquireRadio(RADIO_STATE_STA, 5000); // wait out any geo-scan, claim radio
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
            _AcquireRadio(RADIO_STATE_STA, 5000); // wait out any geo-scan, claim radio
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
            _AcquireRadio(RADIO_STATE_AP, 5000); // wait out any geo-scan, claim radio
            WiFi.mode(WIFI_AP);
            auto result = WiFi.softAP(ApSSID().c_str(), ApPassword().c_str());
            if (!result)
            {
                // Couldn't bring the AP up; hand the radio back to OFF.
                ReleaseWiFi();
            }
            return result;
        }

        static void StopAccessPoint()
        {
            ReleaseWiFi();
        }

    private:
        // Waits (briefly) for an in-flight geolocation scan to release the
        // radio, then records `newState` as the owner so TryAcquireForScan()
        // defers. Does not change the WiFi mode itself — callers apply their
        // own bring-up (softAP/begin/WIFI_OFF) afterward. After maxScanWaitMs
        // the scan's ownership is revoked and taken over so acquisition can't
        // block indefinitely.
        static void _AcquireRadio(WiFiRadioState newState, uint32_t maxScanWaitMs)
        {
            uint32_t start = millis();

            for (;;)
            {
                {
                    RadioLock lock;
                    if (!_ScanOwnsRadio())
                    {
                        RadioState() = newState;
                        return;
                    }
                }

                if (millis() - start >= maxScanWaitMs)
                {
                    // Scan overran its expected duration; take the radio anyway
                    // so the acquiring session stays responsive.
                    RadioLock lock;
                    _ScanOwnsRadio() = false;
                    RadioState() = newState;
                    return;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // Serialises radio-ownership transitions across the geolocation poll
        // task and the UI task (BLE / RPC windows) so acquire/release are
        // atomic check-and-set operations, not racy read-modify-writes.
        static SemaphoreHandle_t &_RadioMutex()
        {
            static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
            return mutex;
        }

        // RAII wrapper around _RadioMutex().
        struct RadioLock
        {
            RadioLock() { xSemaphoreTake(_RadioMutex(), portMAX_DELAY); }
            ~RadioLock() { xSemaphoreGive(_RadioMutex()); }
        };

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