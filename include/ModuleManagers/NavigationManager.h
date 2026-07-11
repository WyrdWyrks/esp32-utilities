#pragma once

#include "CompassInterface.h"
#include "TinyGPS++.h"
#include "NavigationUtils.h"
#include "FilesystemUtils.h"

namespace
{
    const char *LOCATION_FILE PROGMEM = "/SavedLocations.msgpk";
}

namespace NavigationModule
{
    class Manager
    {
    private:
        static constexpr const char* TAG = "NavigationManager";
        bool _pollingStarted = false;

    public:
        Manager() {}

        void InitializeUtils(CompassInterface* compass)
        {
            ESP_LOGI(TAG, "NavigationModule::Manager::InitializeUtils");
            NavigationModule::Utilities::Init(compass);

            NavigationModule::Utilities::SavedLocationsUpdated() += SaveLocationsToFlash;
            this->LoadLocationsFromFlash();

            JsonDocument calibrationData;
            auto returncode = FilesystemModule::Utilities::ReadFile(
                NavigationModule::Utilities::GetCalibrationFilename(), calibrationData);
            if (returncode == FilesystemModule::FilesystemReturnCode::FILESYSTEM_OK)
            {
                NavigationModule::Utilities::SetCalibrationData(calibrationData);

                std::string buf;
                serializeJson(calibrationData, buf);
                ESP_LOGI(TAG, "Calibration data loaded from flash: %s", buf.c_str());
            }
        }
        // Starts a background task that periodically refreshes the location
        // cache so consumers of NavigationUtils::GetCurrentLocation receive the
        // cached fix instead of querying the sources inline. Safe to call once;
        // subsequent calls are ignored.
        void StartLocationPolling(uint32_t intervalMs = 15000,
                                  uint32_t maxAgeMs   = 60000)
        {
            if (_pollingStarted)
            {
                return;
            }

            NavigationModule::Utilities::SetPollIntervalMs(intervalMs);
            NavigationModule::Utilities::SetLocationMaxAge(maxAgeMs);
            NavigationModule::Utilities::EnableLocationCache(true);

            System_Utils::registerTask([](void*) {
                // Own handle registered so RequestSourceRefresh() can wake
                // this task early (e.g. from the geolocation debug screen)
                // instead of waiting out the full poll interval.
                NavigationModule::Utilities::_SetPollTaskHandle(xTaskGetCurrentTaskHandle());

                for (;;)
                {
                    NavigationModule::Utilities::ServicePollCycle();
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(
                        NavigationModule::Utilities::PollIntervalMs()));
                }
            }, "Location Poll", 4096, nullptr, 1);

            _pollingStarted = true;
            ESP_LOGI(TAG, "Started location polling (interval %u ms, max-age %u ms)",
                     intervalMs, maxAgeMs);
        }

        static void SaveLocationsToFlash()
        {
            JsonDocument doc;

            NavigationModule::Utilities::SerializeSavedLocations(doc);
            auto returncode = FilesystemModule::Utilities::WriteFile(LOCATION_FILE, doc);

            if (returncode != FilesystemModule::FilesystemReturnCode::FILESYSTEM_OK)
            {
                ESP_LOGE(TAG, "Failed to save locations to flash. Error code: %d", (int)returncode);
            }
            else
            {
                ESP_LOGI(TAG, "Saved locations to flash");
            }
        }

        void LoadLocationsFromFlash()
        {
            JsonDocument doc;
            auto returncode = FilesystemModule::Utilities::ReadFile(LOCATION_FILE, doc);

            if (returncode != FilesystemModule::FilesystemReturnCode::FILESYSTEM_OK)
            {
                ESP_LOGE(TAG, "Failed to load locations from flash. Error code: %d", (int)returncode);
            }
            else
            {
                NavigationModule::Utilities::DeserializeSavedLocations(doc);
            }
        }
    };
}

// Backward-compatibility alias — prefer NavigationModule::Manager in new code
using NavigationManager = NavigationModule::Manager;
