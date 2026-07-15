#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include "Adafruit_GFX.h"
#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "driver/adc.h"
#include "EventHandler.h"
#include <ezTime.h>
#include "TimeSourceInterface.hpp"

#include "esp_ota_ops.h"
#include "mbedtls/base64.h"

#include "esp_rom_crc.h"
#include "esp_log.h"

#include "VersionUtils.h"
#include "SettingsInterface.hpp"

// File-scope logging tag. Historically declared in System_Utils.h, where it
// leaked (internal linkage) into every translation unit that pulled the header
// in. A number of sibling headers log under this bare `TAG`, so it is kept at
// file scope for backwards compatibility. [[maybe_unused]] silences the
// "defined but not used" warning in the TUs that don't reference it.
[[maybe_unused]] static const char *TAG = "System_Utils";

// -----------------------------------------------------------------------------
// SystemModule::Utilities
// -----------------------------------------------------------------------------
// Header-only consolidation of the former System_Utils.h / System_Utils.cpp.
// All state is held in C++17 `inline static` members so there is no translation
// unit that "owns" the definitions. The legacy global name `System_Utils`
// remains available via the alias at the bottom of this file.
// -----------------------------------------------------------------------------

namespace SystemModule
{
    class Utilities
    {
    public:
        enum DebugCommand
        {
            DISPLAY_CONTENTS = 0,
            REGISTER_INPUT   = 1,
        };

        inline static std::string DeviceName = "ESP32";
        inline static size_t      DeviceID   = 0;

        inline static bool silentMode = true;
        inline static bool time24Hour = false;

        // ---------------------------------------------------------------------
        // Battery
        // ---------------------------------------------------------------------
        static long getBatteryPercentage()
        {
            if (_batteryCallback)
                return _batteryCallback();
            return 0;
        }

        static void registerBatteryCallback(std::function<long()> fn)
        {
            _batteryCallback = fn;
        }

        static void shutdownBatteryWarning()
        {
            systemShutdown.Invoke();
        }

        // ---------------------------------------------------------------------
        // Timer functionality
        // ---------------------------------------------------------------------
        static int registerTimer(const char *timerName, size_t periodMS, TimerCallbackFunction_t callback)
        {
            ESP_LOGV(TAG, "Registering timer: %s", timerName);

            TimerHandle_t handle = xTimerCreate(timerName, periodMS, pdTRUE, (void *)0, callback);

            if (handle != nullptr)
            {
                systemTimers[nextTimerID] = handle;
                return nextTimerID++;
            }
            else
            {
                return -1;
            }
        }

        static int registerTimer(const char *timerName, size_t periodMS, TimerCallbackFunction_t callback, StaticTimer_t &timerBuffer)
        {
            ESP_LOGV(TAG, "Registering static timer: %s", timerName);
            TimerHandle_t handle = xTimerCreateStatic(timerName, periodMS, pdTRUE, (void *)0, callback, &timerBuffer);

            if (handle != nullptr)
            {
                systemTimers[nextTimerID] = handle;
                return nextTimerID++;
            }
            else
            {
                return -1;
            }
        }

        static void deleteTimer(int timerID)
        {
            ESP_LOGV(TAG, "Deleting timer: %d", timerID);

            if (systemTimers.find(timerID) != systemTimers.end())
            {
                xTimerDelete(systemTimers[timerID], 0);
                systemTimers.erase(timerID);
            }
        }

        static bool isTimerActive(int timerID)
        {
            ESP_LOGV(TAG, "Checking if timer is active: %d", timerID);
            if (systemTimers.find(timerID) != systemTimers.end())
            {
                return xTimerIsTimerActive(systemTimers[timerID]);
            }
            return false;
        }

        static void startTimer(int timerID)
        {
            ESP_LOGV(TAG, "Starting timer: %d", timerID);

            if (systemTimers.find(timerID) != systemTimers.end())
            {
                xTimerStart(systemTimers[timerID], 1000);
            }
        }

        static void stopTimer(int timerID)
        {
            ESP_LOGV(TAG, "Stopping timer: %d", timerID);

            if (systemTimers.find(timerID) != systemTimers.end())
            {
                if (xTimerStop(systemTimers[timerID], 1000) == pdFAIL)
                {
                    ESP_LOGE(TAG, "Error stopping timer");
                }
            }
            else
            {
                ESP_LOGE(TAG, "unable to find timer");
            }
        }

        static void resetTimer(int timerID)
        {
            ESP_LOGV(TAG, "Resetting timer: %d", timerID);

            if (systemTimers.find(timerID) != systemTimers.end())
            {
                xTimerReset(systemTimers[timerID], 0);
            }
        }

        static void changeTimerPeriod(int timerID, size_t timerPeriodMS)
        {
            ESP_LOGV(TAG, "Changing timer period: %d", timerID);

            if (systemTimers.find(timerID) != systemTimers.end())
            {
                xTimerChangePeriod(systemTimers[timerID], pdMS_TO_TICKS(timerPeriodMS), 0);
            }
        }

        // ---------------------------------------------------------------------
        // Queue functionality
        // ---------------------------------------------------------------------
        static int registerQueue(size_t queueLength, size_t itemSize)
        {
            QueueHandle_t handle = xQueueCreate(queueLength, itemSize);

            if (handle != nullptr)
            {
                systemQueues[nextQueueID] = handle;
                return nextQueueID++;
            }
            else
            {
                return -1;
            }
        }

        static int registerQueue(size_t queueLength, size_t itemSize, uint8_t *queueData, StaticQueue_t &queueBuffer)
        {
            QueueHandle_t handle = xQueueCreateStatic(queueLength, itemSize, queueData, &queueBuffer);

            if (handle != nullptr)
            {
                systemQueues[nextQueueID] = handle;
                return nextQueueID++;
            }
            else
            {
                return -1;
            }
        }

        static QueueHandle_t getQueue(int queueID)
        {
            if (systemQueues.find(queueID) != systemQueues.end())
            {
                return systemQueues[queueID];
            }
            else
            {
                return nullptr;
            }
        }

        static void deleteQueue(int queueID)
        {
            if (systemQueues.find(queueID) != systemQueues.end())
            {
                vQueueDelete(systemQueues[queueID]);
                systemQueues.erase(queueID);
            }
        }

        static void resetQueue(int queueID)
        {
            if (systemQueues.find(queueID) != systemQueues.end())
            {
                xQueueReset(systemQueues[queueID]);
            }
        }

        static bool sendToQueue(int queueID, void *item, size_t timeoutMS)
        {
            if (systemQueues.find(queueID) != systemQueues.end())
            {
                return xQueueSend(systemQueues[queueID], item, pdMS_TO_TICKS(timeoutMS)) == pdPASS;
            }
            else
            {
                return false;
            }
        }

        // ---------------------------------------------------------------------
        // Task functionality
        // ---------------------------------------------------------------------
        // Dynamic memory allocation, no pinned core
        static int registerTask(
            TaskFunction_t taskFunction,
            const char *taskName,
            uint32_t taskStackSize,
            void *taskParameters,
            UBaseType_t taskPriority)
        {
            TaskHandle_t handle;
            BaseType_t status = xTaskCreate(taskFunction, taskName, taskStackSize, taskParameters, taskPriority, &handle);

            if (status == pdPASS)
            {
                systemTasks[nextTaskID] = handle;
                return nextTaskID++;
            }
            else
            {
                return -1;
            }
        }

        // Dynamic memory allocation, pinned to core
        static int registerTask(
            TaskFunction_t taskFunction,
            const char *taskName,
            uint32_t taskStackSize,
            void *taskParameters,
            UBaseType_t taskPriority,
            BaseType_t coreID)
        {
            TaskHandle_t handle;
            BaseType_t status = xTaskCreatePinnedToCore(taskFunction, taskName, taskStackSize, taskParameters, taskPriority, &handle, coreID);

            if (status == pdPASS)
            {
                systemTasks[nextTaskID] = handle;
                return nextTaskID++;
            }
            else
            {
                ESP_LOGE(TAG, "Unable to create task: %s", taskName);
                return -1;
            }
        }

        // Static memory allocation, no pinned core
        static int registerTask(
            TaskFunction_t taskFunction,
            const char *taskName,
            uint32_t taskStackSize,
            void *taskParameters,
            UBaseType_t taskPriority,
            StackType_t &stackBuffer,
            StaticTask_t &taskBuffer)
        {
            auto handle = xTaskCreateStatic(taskFunction, taskName, taskStackSize, taskParameters, taskPriority, &stackBuffer, &taskBuffer);

            if (handle != nullptr)
            {
                systemTasks[nextTaskID] = handle;
                return nextTaskID++;
            }
            else
            {
                return -1;
            }
        }

        // Static memory allocation, pinned to core
        static int registerTask(
            TaskFunction_t taskFunction,
            const char *taskName,
            uint32_t taskStackSize,
            void *taskParameters,
            UBaseType_t taskPriority,
            StackType_t &stackBuffer,
            StaticTask_t &taskBuffer,
            BaseType_t coreID)
        {
            auto handle = xTaskCreateStaticPinnedToCore(taskFunction, taskName, taskStackSize, taskParameters, taskPriority, &stackBuffer, &taskBuffer, coreID);

            if (handle != nullptr)
            {
                systemTasks[nextTaskID] = handle;
                return nextTaskID++;
            }
            else
            {
                return -1;
            }
        }

        static void suspendTask(int taskID)
        {
            if (systemTasks.find(taskID) != systemTasks.end())
            {
                vTaskSuspend(systemTasks[taskID]);
            }
        }

        static void resumeTask(int taskID)
        {
            if (systemTasks.find(taskID) != systemTasks.end())
            {
                vTaskResume(systemTasks[taskID]);
            }
        }

        static void deleteTask(int taskID)
        {
            if (systemTasks.find(taskID) != systemTasks.end())
            {
                vTaskDelete(systemTasks[taskID]);
                systemTasks.erase(taskID);
            }
        }

        static TaskHandle_t getTask(int taskID)
        {
            if (systemTasks.find(taskID) != systemTasks.end())
            {
                return systemTasks[taskID];
            }
            else
            {
                return nullptr;
            }
        }

        // ---------------------------------------------------------------------
        // WiFi functionality
        // ---------------------------------------------------------------------
        // TODO: kill this
        static bool enableWiFi()
        {
            enableRadio();
            WiFi.disconnect(false);  // Reconnect the network
            WiFi.mode(WIFI_STA);    // Switch WiFi on

            WiFi.begin("ESP32-OTA", "e65v41ev");

            size_t timeoutCounter = 0;
            while (WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (timeoutCounter++ > 20)
                {
                    return false;
                }
            }

            return true;
        }

        static void disableWiFi()
        {
            disableRadio();
            WiFi.disconnect(true);  // Disconnect from the network
            WiFi.mode(WIFI_OFF);    // Switch WiFi off
        }

        static IPAddress getLocalIP()
        {
            return WiFi.localIP();
        }

        // ---------------------------------------------------------------------
        // 2.4Ghz Radio functionality
        // ---------------------------------------------------------------------
        static void enableRadio()
        {
            WiFi.setSleep(false);
        }

        static void disableRadio()
        {
            WiFi.setSleep(true);
        }

        // ---------------------------------------------------------------------
        // RPC OTA
        // ---------------------------------------------------------------------
        static int DecodeBase64(const char* input, uint8_t* output, size_t output_len)
        {
            size_t out_len = 0;
            int err = mbedtls_base64_decode(output, output_len, &out_len,
                                            reinterpret_cast<const unsigned char*>(input),
                                            strlen(input));
            return err == 0 ? out_len : -1;
        }

        static void StartOtaRpc(JsonDocument &doc)
        {
            size_t size = doc["size"] | 0;
            doc.clear();

            if (size == 0) {
                doc["error"] = "Missing or invalid 'size'";
                return;
            }

            ota_state.partition = esp_ota_get_next_update_partition(nullptr);
            if (!ota_state.partition) {
                doc["error"] = "No update partition available";
                return;
            }

            esp_err_t err = esp_ota_begin(ota_state.partition, size, &ota_state.handle);
            if (err != ESP_OK) {
                doc["error"] = "esp_ota_begin failed";
                doc["code"] = err;
                return;
            }

            ota_state.total_size = size;
            ota_state.bytes_written = 0;
            ota_state.active = true;

            doc["status"] = "OTA begin successful";
        }

        static void UploadOtaChunkRpc(JsonDocument &doc)
        {
            std::string buf;
            serializeJsonPretty(doc, buf);
            ESP_LOGV(TAG, "UploadOtaChunkRpc packet: %s", buf.c_str());

            if (doc["chunk"].isNull()) {
                doc.clear();
                doc["error"] = "Missing or invalid 'chunk'";
                return;
            }
            auto b64 = doc["chunk"].as<std::string>();

            if (doc["checksum"].isNull()) {
                doc.clear();
                doc["error"] = "Missing or invalid 'checksum'";
                return;
            }
            auto checksum = doc["checksum"].as<uint32_t>();


            if (!ota_state.active) {
                doc.clear();
                doc["error"] = "OTA inactive";
                return;
            }

            size_t b64Len = strlen(b64.c_str());
            size_t binLen = (b64Len * 3) / 4;
            std::unique_ptr<uint8_t[]> buffer(new uint8_t[binLen]);

            int actualLen = DecodeBase64(b64.c_str(), buffer.get(), binLen);
            if (actualLen <= 0) {
                doc.clear();
                doc["error"] = "Base64 decode failed";
                return;
            }

            doc.clear();

            uint32_t calculatedChecksum = 0;
            for (size_t i = 0; i < actualLen; i++) {
                calculatedChecksum += buffer[i];
            }

            if (calculatedChecksum != checksum) {
                doc["error"] = "CRC mismatch";
                ESP_LOGD(TAG, "expected checksum: %08X", checksum);
                ESP_LOGD(TAG, "calculated checksum: %08X", calculatedChecksum);
                return;
            }

            esp_err_t err = esp_ota_write(ota_state.handle, buffer.get(), actualLen);
            if (err != ESP_OK) {
                esp_ota_abort(ota_state.handle);
                ota_state.active = false;
                doc["error"] = "esp_ota_write failed";
                doc["code"] = err;
                return;
            }

            ota_state.bytes_written += actualLen;
            doc["written"] = actualLen;
            doc["total_written"] = ota_state.bytes_written;
            doc["remaining"] = ota_state.total_size - ota_state.bytes_written;
        }

        static void EndOtaRpc(JsonDocument &doc)
        {
            doc.clear();

            if (!ota_state.active) {
                doc["error"] = "OTA not active";
                return;
            }

            if (ota_state.bytes_written < ota_state.total_size) {
                doc["error"] = "Not enough data written";
                return;
            }

            esp_err_t err = esp_ota_end(ota_state.handle);
            if (err != ESP_OK) {
                doc["error"] = "esp_ota_end failed";
                doc["code"] = err;
                return;
            }

            err = esp_ota_set_boot_partition(ota_state.partition);
            if (err != ESP_OK) {
                doc["error"] = "esp_ota_set_boot_partition failed";
                doc["code"] = err;
                return;
            }

            ota_state.active = false;
            doc["status"] = "OTA complete";
        }

        // ---------------------------------------------------------------------
        // Debug Companion Functionality
        // ---------------------------------------------------------------------
        static void GetSystemInfoRpc(JsonDocument &doc)
        {
            ESP_LOGV(TAG, "GetSystemInfoRpc called");
            doc["DeviceName"] = DeviceName;
            doc["DeviceID"] = DeviceID;
            doc["FirmwareVersion"] = FIRMWARE_VERSION_STRING;
            #ifdef HARDWARE_VERSION
            doc["HardwareVersion"] = HARDWARE_VERSION;
            #else
            doc["HardwareVersion"] = 0;
            #endif
        }

        // ---------------------------------------------------------------------
        // Event Handler public invoke functions
        // ---------------------------------------------------------------------
        static void enableInterruptsInvoke()
        {
            ESP_LOGV(TAG, "Enabling interrupts");
            enableInterrupts.Invoke();
        }

        static void disableInterruptsInvoke()
        {
            ESP_LOGV(TAG, "Disabling interrupts");
            disableInterrupts.Invoke();
        }

        static void systemShutdownInvoke()
        {
            ESP_LOGV(TAG, "Shutting down system");
            systemShutdown.Invoke();
        }

        static void enablePowerSavingsInvoke()
        {
            ESP_LOGV(TAG, "Enabling power savings");
            enablePowerSavings.Invoke();
        }

        static void disablePowerSavingsInvoke()
        {
            ESP_LOGV(TAG, "Disabling power savings");
            disablePowerSavings.Invoke();
        }

        // ---------------------------------------------------------------------
        // Event Handler Getters
        // ---------------------------------------------------------------------
        static EventHandler<> &getEnableInterrupts() { return enableInterrupts; }
        static EventHandler<> &getDisableInterrupts() { return disableInterrupts; }
        static EventHandler<> &getSystemShutdown() { return systemShutdown; }
        static EventHandler<> &getEnablePowerSavings() { return enablePowerSavings; }
        static EventHandler<> &getDisablePowerSavings() { return disablePowerSavings; }

        static void UpdateSettings(JsonDocument &settings)
        {
            if (!settings["UserID"].isNull())
            {
                DeviceID = 0 | settings["UserID"].as<int>();
            }

            if (!settings["Device Name"].isNull())
            {
                DeviceName = settings["Device Name"].as<std::string>();
            }

            if (!settings["Silent Mode"].isNull())
            {
                silentMode = settings["Silent Mode"].as<bool>();
                ESP_LOGI(TAG, "Assigning silentMode %d", silentMode);
            }

            if (!settings["24H Time"].isNull())
            {
                time24Hour = settings["24H Time"].as<bool>();
            }

            if (!settings["Timezone"].isNull())
            {
                const char* posix = _PosixForTimezone(settings["Timezone"].as<int>());
                LocalTimezone().setPosix(posix);
                ESP_LOGI(TAG, "Timezone set to %s", posix);
            }
        }

        static void GenerateDefaultSettings(std::vector<std::shared_ptr<FilesystemModule::SettingsInterface>> &settings)
        {
            auto silentMode = std::make_shared<FilesystemModule::BoolSetting>("Silent Mode", false);
            settings.push_back(silentMode);

            auto time24hr = std::make_shared<FilesystemModule::BoolSetting>("24H Time", false);
            settings.push_back(time24hr);

            auto timezone = std::make_shared<FilesystemModule::EnumSetting>(
                "Timezone",
                1,
                std::vector<std::string>{
                    "UTC",
                    "US/Eastern",
                    "US/Central",
                    "US/Mountain",
                    "US/Pacific",
                    "US/Alaska",
                    "US/Hawaii",
                    "Europe/London",
                    "Europe/Central",
                    "Europe/Eastern",
                    "Asia/Dubai",
                    "Asia/Kolkata",
                    "Asia/Bangkok",
                    "Asia/Shanghai",
                    "Asia/Tokyo",
                    "Australia/Sydney",
                },
                std::vector<int>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }
            );
            settings.push_back(timezone);
        }

        // ---------------------------------------------------------------------
        // Time Management
        // ---------------------------------------------------------------------
        static void RegisterTimeSource(SystemModule::TimeSourceInterface* source)
        {
            TimeSources().push_back(source);
        }

        static bool GetCurrentUTC(time_t& outTime)
        {
            for (auto* src : TimeSources()) {
                if (src->TryGetCurrentUTC(outTime)) {
                    UTC.setTime(outTime);
                    return true;
                }
            }
            return false;
        }

        static time_t GetCurrentLocal()
        {
            return LocalTimezone().now();
        }

        static bool IsTimeValid()
        {
            time_t dummy = 0;
            return GetCurrentUTC(dummy);
        }

        static std::string FormatTime(time_t t)
        {
            return std::string(LocalTimezone().dateTime(t, time24Hour ? "H:i" : "g:i A").c_str());
        }

        static std::string FormatDate(time_t t)
        {
            return std::string(LocalTimezone().dateTime(t, "n/j/Y").c_str());
        }

        static std::vector<SystemModule::TimeSourceInterface*>& TimeSources()
        {
            static std::vector<SystemModule::TimeSourceInterface*> sources;
            return sources;
        }

        static Timezone& LocalTimezone()
        {
            static Timezone tz;
            return tz;
        }

        // DEBUGGING FUNCTIONS
        static void PrintHeapFragmentation() {
            // Fragmentation: 1 - (maxContiguous / freeHeap). 0% means all free
            // memory is one contiguous block; ~100% means it is heavily split up.
            float frag = 1.0f - static_cast<float>(ESP.getMaxAllocHeap())
                                / static_cast<float>(ESP.getFreeHeap());
            ESP_LOGI(TAG, "Heap fragmentation: %.2f%%", frag * 100);
        }

    private:
        static constexpr const char* COMMAND_FIELD        = "cmd";
        static constexpr const char* DISPLAY_BUFFER_FIELD = "buffer";
        static constexpr const char* DISPLAY_WIDTH        = "width";
        static constexpr const char* DISPLAY_HEIGHT       = "height";

        static const char* _PosixForTimezone(int id)
        {
            static const std::unordered_map<int, const char*> timezones =
            {
                { 0,  "UTC0"                               },  // UTC
                { 1,  "EST5EDT,M3.2.0,M11.1.0"            },  // US/Eastern
                { 2,  "CST6CDT,M3.2.0,M11.1.0"            },  // US/Central
                { 3,  "MST7MDT,M3.2.0,M11.1.0"            },  // US/Mountain
                { 4,  "PST8PDT,M3.2.0,M11.1.0"            },  // US/Pacific
                { 5,  "AKST9AKDT,M3.2.0,M11.1.0"          },  // US/Alaska
                { 6,  "HST10"                              },  // US/Hawaii
                { 7,  "GMT0BST,M3.5.0/1,M10.5.0"          },  // Europe/London
                { 8,  "CET-1CEST,M3.5.0,M10.5.0/3"        },  // Europe/Central
                { 9,  "EET-2EEST,M3.5.0/3,M10.5.0/4"      },  // Europe/Eastern
                { 10, "GST-4"                              },  // Asia/Dubai
                { 11, "IST-5:30"                           },  // Asia/Kolkata
                { 12, "ICT-7"                              },  // Asia/Bangkok
                { 13, "CST-8"                              },  // Asia/Shanghai
                { 14, "JST-9"                              },  // Asia/Tokyo
                { 15, "AEST-10AEDT,M10.1.0,M4.1.0/3"      },  // Australia/Sydney
            };

            auto it = timezones.find(id);
            return it != timezones.end() ? it->second : "UTC0";
        }

        // OTA progress state, shared across the OTA RPC handlers.
        struct OtaState
        {
            esp_ota_handle_t       handle;
            const esp_partition_t* partition;
            size_t                 total_size;
            size_t                 bytes_written;
            bool                   active;
        };
        inline static OtaState ota_state{};

        // Event Handlers
        inline static EventHandler<> enableInterrupts;
        inline static EventHandler<> disableInterrupts;
        inline static EventHandler<> systemShutdown;
        inline static EventHandler<> enablePowerSavings;
        inline static EventHandler<> disablePowerSavings;

        // Timer functionality
        inline static std::unordered_map<int, TimerHandle_t> systemTimers;
        inline static int nextTimerID = 0;

        // Task functionality
        inline static std::unordered_map<int, TaskHandle_t> systemTasks;
        inline static int nextTaskID = 0;

        // Queue functionality
        inline static std::unordered_map<int, QueueHandle_t> systemQueues;
        inline static int nextQueueID = 0;

        // Battery callback
        inline static std::function<long()> _batteryCallback;

        // ADC Users
        inline static std::unordered_map<uint8_t, bool> adcUsers;

        inline static int otaTaskID = -1;
    };
}

// Backwards-compatible alias: the class used to live in the global namespace as
// `System_Utils`. Existing call sites (System_Utils::Foo) keep working.
using System_Utils = SystemModule::Utilities;
