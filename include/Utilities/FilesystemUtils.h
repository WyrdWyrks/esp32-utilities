#pragma once

#include "ArduinoJson.h"
#include "SystemUtilities.hpp"
#include "SettingsInterface.hpp"
#include <StreamUtils.h>
#include <SPIFFS.h>
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace FilesystemModule
{
    static const char *TAG = "FilesystemModule";

    enum FilesystemReturnCode
    {
        FILESYSTEM_OK = 0,
        READ_BUFFER_OVERFLOW = 1,
        FILE_NOT_FOUND = 2,
        WRITE_FAILED = 3,
        READ_ERROR = 4
    };

    // Static class with helper functions for interacting with the SPIFFS filesystem
    // Data is stored in MessagePack format
    class Utilities
    {
    public:
        // Initializes the SPIFFS filesystem. Formats the filesystem if it doesn't exist and reboots the device
        static void Init()
        {
            if (!SPIFFS.begin(true))
            {
                ESP_LOGE(TAG, "SPIFFS Mount Failed. Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP.restart();
            }

        }

        // Reads a file from the SPIFFS filesystem into a JsonDocument
        static FilesystemReturnCode ReadFile(std::string filename, JsonDocument &doc)
        {
            ESP_LOGD(TAG, "Reading file: %s", filename.c_str());

            if (!SPIFFS.exists(filename.c_str()))
            {
                ESP_LOGW(TAG, "File not found: %s", filename.c_str());
                return FilesystemReturnCode::FILE_NOT_FOUND;
            }

            File file = SPIFFS.open(filename.c_str(), FILE_READ);

            if (!file)
            {
                ESP_LOGE(TAG, "Failed to open file: %s", filename.c_str());
                return FilesystemReturnCode::READ_ERROR;
            }

            deserializeMsgPack(doc, file);
            file.close();

            if (doc.overflowed())
            {
                ESP_LOGE(TAG, "Buffer overflow while reading file: %s", filename.c_str());
                return FilesystemReturnCode::READ_BUFFER_OVERFLOW;
            }

            if (doc.isNull())
            {
                ESP_LOGE(TAG, "Failed to deserialize file: %s", filename.c_str());
                return FilesystemReturnCode::READ_ERROR;
            }
            else
            {
                std::string buf;
                serializeJson(doc, buf);
                ESP_LOGV(TAG, "File read successfully: %s, content: %s", filename.c_str(), buf.c_str());
            }
            return FilesystemReturnCode::FILESYSTEM_OK;
        }

        // Writes a file to the SPIFFS filesystem from a JsonDocument
        static FilesystemReturnCode WriteFile(std::string filename, JsonDocument &doc)
        {
            std::string buf;
            serializeJson(doc, buf);
            ESP_LOGV(TAG, "Writing file: %s, content: %s", filename.c_str(), buf.c_str());

            File file = SPIFFS.open(filename.c_str(), FILE_WRITE);
            if (!file)
            {
                return FilesystemReturnCode::WRITE_FAILED;
            }

            auto bytesWritten = serializeMsgPack(doc, file);
            file.close();

            if (bytesWritten == 0)
            {
                return FilesystemReturnCode::WRITE_FAILED;
            }

            return FilesystemReturnCode::FILESYSTEM_OK;
        }

        // =============== Settings Management ===============

        static SettingsMap &DeviceSettings()
        {
            static SettingsMap _DeviceSettings;
            return _DeviceSettings;
        }

        static std::shared_ptr<FilesystemModule::SettingsInterface> FindSetting(const std::string &key)
        {
            auto &s = DeviceSettings();
            auto it = std::find_if(s.begin(), s.end(),
                [&key](const std::shared_ptr<FilesystemModule::SettingsInterface> &x) { return x->key == key; });
            return (it != s.end()) ? *it : nullptr;
        }

        static Preferences &SettingsPreference()
        {
            static Preferences _SettingsPreference;
            return _SettingsPreference;
        }

        static Preferences &DeviceInfo()
        {
            static Preferences _DeviceInfo;
            return _DeviceInfo;
        }

        static void RpcGetSettingsFile(JsonDocument &doc)
        {
            doc.clear();

            for (auto &setting : DeviceSettings())
            {
                auto obj = doc[setting->key].to<ArduinoJson::JsonObject>();
                setting->toJson(obj);
            }
        }

        // ================ Fetch Settings Helpers ================

        static bool FetchBoolSetting(std::string key, bool defaultVal = false)
        {
            auto setting = FindSetting(key);
            if (setting && setting->getType() == "bool")
            {
                return std::static_pointer_cast<BoolSetting>(setting)->value;
            }
            return defaultVal;
        }

        static int FetchIntSetting(std::string key, int defaultVal = 0)
        {
            auto setting = FindSetting(key);
            if (setting && setting->getType() == "int")
            {
                return std::static_pointer_cast<IntSetting>(setting)->value;
            }
            return defaultVal;
        }

        static float FetchFloatSetting(std::string key, float defaultVal = 0.0f)
        {
            auto setting = FindSetting(key);
            if (setting && setting->getType() == "float")
            {
                return std::static_pointer_cast<FloatSetting>(setting)->value;
            }
            return defaultVal;
        }

        static std::string FetchStringSetting(std::string key, std::string defaultVal = "")
        {
            auto setting = FindSetting(key);
            if (setting && setting->getType() == "string")
            {
                return std::static_pointer_cast<StringSetting>(setting)->value;
            }
            return defaultVal;
        }

        static int FetchEnumSetting(std::string key, int defaultVal = 0)
        {
            auto setting = FindSetting(key);
            if (setting && setting->getType() == "enum")
            {
                return std::static_pointer_cast<EnumSetting>(setting)->value;
            }
            return defaultVal;
        }

        // ================ Write Settings Helpers ================

        

        // ================ Rpc Functions ================

        static void RpcUpdateSettingsFile(JsonDocument &doc)
        {
            // _SettingsFile.set(doc["Settings"].as<JsonObject>());
            // WriteSettingsFileToFlash();

            ArduinoJson::serializeJson(doc["Settings"], Serial);

            doc.clear();
            doc["Success"] = true;
        }

        static void RpcUpdateSetting(JsonDocument &doc)
        {
            auto key = doc["SettingKey"].as<std::string>();
            auto setting = FindSetting(key);

            if (setting)
            {
                doc[SettingsInterface::write_key] = doc["SettingValue"];
                auto obj = doc.as<ArduinoJson::JsonObjectConst>();
                setting->fromJson(obj);
                setting->saveToPreferences(SettingsPreference());
                ESP_LOGI(TAG, "Updated setting: %s to %s", key.c_str(), doc["SettingValue"].as<const char*>());

                doc.clear();
                doc["Success"] = true;
            }
            else
            {
                ESP_LOGW(TAG, "Setting not found: %s", key.c_str());
                doc.clear();
                doc["Success"] = false;
                return;
            }
        }

        static void RpcUpdateSettings(JsonDocument &doc)
        {
            bool success = true;

            if (!doc["Settings"].isNull() && doc["Settings"].is<JsonArray>())
            {
                // Open once so the whole batch writes in a single transaction;
                // each saveToPreferences reuses this handle.
                SettingsPreference().begin(SettingsInterface::preference_namespace, false);
                Preferences& prefs = SettingsPreference();
                auto settingsArray = doc["Settings"].as<JsonArray>();

                for (JsonVariant entry : settingsArray)
                {
                    auto key = entry["SettingKey"].as<std::string>();
                    auto s = FindSetting(key);

                    if (!s)
                    {
                        ESP_LOGW(TAG, "Setting not found: %s", key.c_str());
                        success = false;
                        continue;
                    }

                    entry[SettingsInterface::write_key] = entry["SettingValue"];
                    auto obj = entry.as<JsonObjectConst>();
                    s->fromJson(obj);
                    s->saveToPreferences(prefs);
                    ESP_LOGI(TAG, "Updated setting: %s to %s", key.c_str(), entry["SettingValue"].as<const char*>());
                }

                SettingsPreference().end();
            }
            else
            {
                success = false;
            }

            doc.clear();
            doc["Success"] = success;
        }

        static void InvokeSettingsUpdated(SettingsMap &settings, size_t jsonDocSize = 2048)
        {
            JsonDocument doc;

            for (auto &setting : settings)
            {
                setting->toJsonSettingsDoc(doc);
                auto valueStr = doc[setting->key.c_str()].as<std::string>();
                ESP_LOGD(TAG, "Setting %s: %s", setting->key.c_str(), valueStr.c_str());
            }

            _SettingsUpdated.Invoke(doc);
        }

        static EventHandler<JsonDocument &> &SettingsUpdated() { return _SettingsUpdated; }

        static EventHandler<> &RequestSettingsRefresh() 
        {
            static EventHandler<> _requestSettingsRefresh;
            return _requestSettingsRefresh;
        }

    protected:
        // Settings File
        static JsonDocument _SettingsFile;
        static std::string _SettingsFilename;

        // Event handler for settings file updates
        static EventHandler<JsonDocument&> _SettingsUpdated;
    };
};