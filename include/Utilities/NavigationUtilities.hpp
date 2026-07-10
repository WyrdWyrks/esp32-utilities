#pragma once

#include "SystemUtilities.hpp"
#include "CompassInterface.h"
#include "TinyGPS++.h"
#include "GeolocationInterface.hpp"
#include <ezTime.h>
#include <Preferences.h>
#include <string>

namespace
{
    const char *COMPASS_CALIBRATION_FILENAME PROGMEM = "/CompassCalibration.msgpk";
}

namespace NavigationModule
{
    struct SavedLocation
    {
        std::string Name;
        double Latitude;
        double Longitude;
    };

    // Static utility class managing GPS, compass, saved locations, and geolocation sources.
    class Utilities
    {
        static constexpr const char* TAG = "NavigationUtils";
        // Dedicated Preferences namespace for geolocation source enable/disable
        // state, keyed by GeolocationInterface::GetMoniker() (e.g. "gps", "static").
        // NVS namespace names are capped at 15 chars.
        static constexpr const char* GEO_SOURCE_PREFS_NAMESPACE = "GeoSources";

    public:
        static void Init(CompassInterface* compass)
        {
            _Compass() = compass;
        }

        // static void Init(CompassInterface* compass, Stream& gpsInputStream)
        // {
        //     _Compass() = compass;
        //     _GpsInputStream() = &gpsInputStream;
        // }

        // GPS Functionality
        // static void UpdateGPS()
        // {
        //     ESP_LOGI(TAG, "Updating GPS data...");

        //     while (_GpsInputStream()->available() > 0)
        //     {
        //         GetGPS().encode(_GpsInputStream()->read());
        //     }

        //     if (GetGPS().location.isValid())
        //     {
        //         ESP_LOGI(TAG, "Valid GPS location received. Latitude: %f, Longitude: %f",
        //             GetGPS().location.lat(), GetGPS().location.lng());
        //         _LastCoordinate() = GetGPS().location;
        //     }
        //     else
        //     {
        //         ESP_LOGW(TAG, "Invalid GPS location. Satellite count: %d", GetGPS().satellites.value());
        //     }
        // }

        // static bool IsGPSConnected()
        // {
        //     return _LastCoordinate().isValid();
        // }

        // static TinyGPSLocation GetLocation()
        // {
        //     return _LastCoordinate();
        // }

        // static TinyGPSTime GetTime()
        // {
        //     return GetGPS().time;
        // }

        // static TinyGPSDate GetDate()
        // {
        //     return GetGPS().date;
        // }

        // static uint64_t GetTimeDifference(uint32_t time1, uint32_t date1, uint32_t time2, uint32_t date2)
        // {
        //     time_t t1 = PackedToTimeT(time1, date1);
        //     time_t t2 = PackedToTimeT(time2, date2);
        //     time_t diffSec = (t2 >= t1) ? (t2 - t1) : 0;

        //     uint8_t  s  = diffSec % 60; diffSec /= 60;
        //     uint8_t  mn = diffSec % 60; diffSec /= 60;
        //     uint8_t  h  = diffSec % 24; diffSec /= 24;
        //     uint32_t d  = (uint32_t)diffSec;

        //     uint64_t diff = 0;
        //     diff |= (uint64_t)s  << 8;
        //     diff |= (uint64_t)mn << 16;
        //     diff |= (uint64_t)h  << 24;
        //     diff |= (uint64_t)(d & 0xFF) << 32;
        //     return diff;
        // }

        static uint64_t GetTimeDifference(uint32_t time1, uint32_t date1)
        {
            time_t now = 0;
            System_Utils::GetCurrentUTC(now);
            time_t msgTime = PackedToTimeT(time1, date1);
            time_t diffSec = (now >= msgTime) ? (now - msgTime) : 0;

            uint8_t  s  = diffSec % 60; diffSec /= 60;
            uint8_t  mn = diffSec % 60; diffSec /= 60;
            uint8_t  h  = diffSec % 24; diffSec /= 24;
            uint32_t d  = (uint32_t)diffSec;

            uint64_t diff = 0;
            diff |= (uint64_t)s  << 8;
            diff |= (uint64_t)mn << 16;
            diff |= (uint64_t)h  << 24;
            diff |= (uint64_t)(d & 0xFF) << 32;
            return diff;
        }



        static TinyGPSPlus& GetGPS()
        {
            static TinyGPSPlus gps;
            return gps;
        }

        static time_t PackedToTimeT(uint32_t gpsTime, uint32_t gpsDate)
        {
            uint8_t  h  = (gpsTime / 1000000) % 100;
            uint8_t  m  = (gpsTime / 10000)   % 100;
            uint8_t  s  = (gpsTime / 100)     % 100;
            uint8_t  d  = gpsDate % 100;
            uint8_t  mo = (gpsDate / 100)     % 100;
            uint16_t y  = (gpsDate / 10000)   % 100 + 2000;
            return ezt::makeTime(h, m, s, d, mo, y);
        }

        static void TimeTToGpsPacked(time_t t, uint32_t &gpsTime, uint32_t &gpsDate)
        {
            tmElements_t tm;
            ezt::breakTime(t, tm);

            uint8_t  h  = tm.Hour;
            uint8_t  m  = tm.Minute;
            uint8_t  s  = tm.Second;
            uint8_t  d  = tm.Day;
            uint8_t  mo = tm.Month;
            uint16_t y  = tm.Year + 1970;  // tmElements_t years are offset from 1970

            gpsTime = (uint32_t)h  * 1000000
                    + (uint32_t)m  * 10000
                    + (uint32_t)s  * 100;

            gpsDate = (uint32_t)((y - 2000) % 100) * 10000
                    + (uint32_t)mo * 100
                    + (uint32_t)d;
        }

        // // Compass Functionality
        // TODO: Rename all these shitty functions
        static int GetAzimuth()
        {
            if (_Compass() == nullptr)
            {
                return -1;
            }
            return _Compass()->GetAzimuth();
        }

        static float GetBearing(float targetHeading = 360.0f)
        {
            if (!_Compass())
            { 
                return -1.0f;
            }
            float bearing = targetHeading - static_cast<float>(_Compass()->GetAzimuth());
            if (bearing < 0.0f)   { bearing += 360.0f; }
            if (bearing >= 360.0f) { bearing -= 360.0f; }
            return bearing;
        }

        static double GetDistance(double latFrom, double lonFrom, double lat, double lon)
        {
            return GetGPS().distanceBetween(latFrom, lonFrom, lat, lon);
        }

        static double GetDistanceTo(double lat, double lon)
        {
            double latFrom, lonFrom = 0.0;

            if (!GetCurrentLocation(latFrom, lonFrom))
            {
                return -1;
            }

            ESP_LOGV(TAG, "GetDistanceTo()");
            ESP_LOGV(TAG, "My Lat: %f", latFrom);
            ESP_LOGV(TAG, "My Lon: %f", lonFrom);
            ESP_LOGV(TAG, "Target Lat: %f", lat);
            ESP_LOGV(TAG, "Target Lon: %f", lon);

            return GetGPS().distanceBetween(latFrom, lonFrom, lat, lon);
        }

        static double GetHeading(double lat, double lon)
        {
            double latFrom, lonFrom = 0.0;

            if (!GetCurrentLocation(latFrom, lonFrom))
            {
                return -1;
            }

            ESP_LOGI(TAG, "GetHeading()");
            ESP_LOGI(TAG, "My Lat: %f", latFrom);
            ESP_LOGI(TAG, "My Lon: %f", lonFrom                                                                                                                                                                                                                                                                                                                                                          );
            ESP_LOGI(TAG, "Target Lat: %f", lat);
            ESP_LOGI(TAG, "Target Lon: %f", lon);

            return GetGPS().courseTo(latFrom, lonFrom, lat, lon);
        }

        static double GetHeadingTo(double lat, double lon)
        {
            double latFrom, lonFrom = 0.0;

            if (!GetCurrentLocation(latFrom, lonFrom))
            {
                return -1;
            }

            return GetGPS().courseTo(latFrom, lonFrom, lat, lon);
        }

        // Stub implementations (only used for debug screen)
        static int GetX() { return 0; }
        static int GetY() { return 0; }
        static int GetZ() { return 0; }

        static void PrintRawValues()
        {
            if (_Compass())
            {
                _Compass()->PrintRawValues();
            }
        }

        // Compass Calibration
        static void BeginCalibration()
        {
            if (_Compass())
            {
                _Compass()->BeginCalibration();
            }
        }

        static void IterateCalibration()
        {
            if (_Compass())
            {
                _Compass()->IterateCalibration();
            }
        }

        static void EndCalibration()
        {
            if (_Compass())
            {
                _Compass()->EndCalibration();
            }
        }

        static void GetCalibrationData(JsonDocument& doc)
        {
            if (_Compass())
            {
                _Compass()->GetCalibrationData(doc);
            }
        }

        static void SetCalibrationData(JsonDocument& doc)
        {
            if (_Compass())
            {
                _Compass()->SetCalibrationData(doc);
            }
        }

        static const char* GetCalibrationFilename()
        {
            return COMPASS_CALIBRATION_FILENAME;
        }

        // Saved Locations
        static EventHandler<>& SavedLocationsUpdated()
        {
            static EventHandler<> handler;
            return handler;
        }

        static void AddSavedLocation(SavedLocation location, bool updateSavedLocations = true)
        {
            _SavedLocations().push_back(location);

            if (updateSavedLocations)
            {
                SavedLocationsUpdated().Invoke();
            }
        }

        static void RemoveSavedLocation(std::vector<SavedLocation>::iterator& locationIt)
        {
            locationIt = _SavedLocations().erase(locationIt);
            SavedLocationsUpdated().Invoke();
        }

        static void ClearSavedLocations()
        {
            _SavedLocations().clear();
            SavedLocationsUpdated().Invoke();
        }

        static void UpdateSavedLocation(
            std::vector<SavedLocation>::iterator& locationIt,
            SavedLocation location)
        {
            locationIt->Name = location.Name;
            locationIt->Latitude = location.Latitude;
            locationIt->Longitude = location.Longitude;
        }

        static std::vector<SavedLocation>::iterator GetSavedLocationsBegin()
        {
            return _SavedLocations().begin();
        }

        static std::vector<SavedLocation>::iterator GetSavedLocationsEnd()
        {
            return _SavedLocations().end();
        }

        static size_t GetSavedLocationsSize()
        {
            return _SavedLocations().size();
        }

        static void SerializeSavedLocations(JsonDocument& doc)
        {
            JsonArray locationArray;

            if (!doc["Locations"].isNull())
            {
                locationArray = doc["Locations"].as<JsonArray>();
            }
            else
            {
                locationArray = doc["Locations"].to<ArduinoJson::JsonArray>();
            }

            for (auto location : _SavedLocations())
            {
                JsonObject locationObject = locationArray.add<ArduinoJson::JsonObject>();
                locationObject["Name"] = location.Name;
                locationObject["Lat"] = location.Latitude;
                locationObject["Lng"] = location.Longitude;
            }

            std::string buf;
            serializeJson(doc, buf);
            ESP_LOGV(TAG, "Saving location list: %s", buf.c_str());
        }

        static void DeserializeSavedLocations(JsonDocument& doc)
        {
            auto locationArray = doc["Locations"].as<JsonArray>();
            _SavedLocations().clear();

            for (auto location : locationArray)
            {
                SavedLocation savedLocation;
                savedLocation.Name = location["Name"].as<std::string>();
                savedLocation.Latitude = location["Lat"].as<double>();
                savedLocation.Longitude = location["Lng"].as<double>();
                _SavedLocations().push_back(savedLocation);
            }
        }

        // RPC
        static void RpcAddSavedLocation(JsonDocument& doc)
        {
            if (!doc["Name"].isNull() && !doc["Lat"].isNull() && !doc["Lng"].isNull())
            {
                SavedLocation location;
                location.Name = doc["Name"].as<std::string>();
                location.Latitude = doc["Lat"].as<double>();
                location.Longitude = doc["Lng"].as<double>();
                AddSavedLocation(location, true);
            }

            doc.clear();
        }

        static void RpcAddSavedLocations(JsonDocument& doc)
        {
            if (!doc["Locations"].isNull())
            {
                auto locations = doc["Locations"].as<JsonArray>();
                for (auto location : locations)
                {
                    SavedLocation savedLocation;
                    savedLocation.Name = location["Name"].as<std::string>();
                    savedLocation.Latitude = location["Lat"].as<double>();
                    savedLocation.Longitude = location["Lng"].as<double>();
                    AddSavedLocation(savedLocation, false);
                }

                SavedLocationsUpdated().Invoke();
            }

            doc.clear();
        }

        static void RpcRemoveSavedLocation(JsonDocument& doc)
        {
            bool success = false;

            if (!doc["Idx"].isNull())
            {
                auto idx = doc["Idx"].as<int>();
                if (idx >= 0 && idx < (int)_SavedLocations().size())
                {
                    success = true;
                    auto locationIt = _SavedLocations().begin() + idx;
                    RemoveSavedLocation(locationIt);
                }
            }

            doc.clear();
            doc["Success"] = success;
        }

        static void RpcClearSavedLocations(JsonDocument& doc)
        {
            ClearSavedLocations();
            doc.clear();
        }

        static void RpcUpdateSavedLocation(JsonDocument& doc)
        {
            bool success = false;

            if (!doc["Idx"].isNull() && !doc["Name"].isNull() &&
                !doc["Lat"].isNull() && !doc["Lng"].isNull())
            {
                auto idx = doc["Idx"].as<int>();
                if (idx >= 0 && idx < (int)_SavedLocations().size())
                {
                    success = true;
                    auto locationIt = _SavedLocations().begin() + idx;
                    UpdateSavedLocation(locationIt, {
                        doc["Name"].as<std::string>(),
                        doc["Lat"].as<double>(),
                        doc["Lng"].as<double>()
                    });
                }
            }

            doc.clear();
            doc["Success"] = success;
        }

        static void RpcGetSavedLocation(JsonDocument& doc)
        {
            if (!doc["Idx"].isNull())
            {
                auto idx = doc["Idx"].as<int>();
                doc.clear();
                if (idx >= 0 && idx < (int)_SavedLocations().size())
                {
                    auto locationIt = _SavedLocations().begin() + idx;
                    doc["Name"] = locationIt->Name;
                    doc["Lat"] = locationIt->Latitude;
                    doc["Lng"] = locationIt->Longitude;
                }
            }
        }

        static void RpcGetSavedLocations(JsonDocument& doc)
        {
            doc.clear();
            JsonArray locationArray = doc["Locations"].to<ArduinoJson::JsonArray>();
            for (auto location : _SavedLocations())
            {
                JsonObject locationObject = locationArray.add<ArduinoJson::JsonObject>();
                locationObject["Name"] = location.Name;
                locationObject["Lat"] = location.Latitude;
                locationObject["Lng"] = location.Longitude;
            }

            std::string buf;
            serializeJsonPretty(doc, buf);
            ESP_LOGV(TAG, "%s", buf.c_str());
        }

        // static void FlashSampleLocations()
        // {
        //     _SavedLocations().clear();

        //     SavedLocation nyc;
        //     nyc.Name = "NYC";
        //     nyc.Latitude = 40.7128;
        //     nyc.Longitude = -74.0060;
        //     _SavedLocations().push_back(nyc);

        //     SavedLocation sf;
        //     sf.Name = "SF";
        //     sf.Latitude = 37.7749;
        //     sf.Longitude = -122.4194;
        //     _SavedLocations().push_back(sf);

        //     SavedLocation atl;
        //     atl.Name = "ATL";
        //     atl.Latitude = 33.7490;
        //     atl.Longitude = -84.3880;
        //     _SavedLocations().push_back(atl);

        //     SavedLocationsUpdated().Invoke();
        // }

        // Geolocation Source Registry
        static void RegisterLocationSource(GeolocationInterface* source)
        {
            source->enabled = _LoadSourceEnabled(source->GetMoniker());
            LocationSources().push_back(source);
        }

        // Enables/disables a registered source and persists the choice to the
        // GeolocationSources preferences namespace, keyed by GetMoniker().
        static void SetSourceEnabled(GeolocationInterface* source, bool enabled)
        {
            if (!source)
            {
                return;
            }

            source->enabled = enabled;
            _SaveSourceEnabled(source->GetMoniker(), enabled);
        }

        // Looks up a registered source by moniker (see GpsState for an example
        // debug readout) and applies/persists the enabled state.
        static bool SetSourceEnabled(const std::string& moniker, bool enabled)
        {
            for (auto* src : LocationSources())
            {
                if (moniker == src->GetMoniker())
                {
                    SetSourceEnabled(src, enabled);
                    return true;
                }
            }

            ESP_LOGW(TAG, "SetSourceEnabled: no registered source with moniker %s", moniker.c_str());
            return false;
        }

        // Returns the current location. When background polling is enabled (see
        // NavigationManager::StartLocationPolling), this serves the most recent
        // cached fix and returns false if that fix is older than the configured
        // max-age. Otherwise it falls back to the legacy on-demand path that
        // queries each registered source inline.
        static bool GetCurrentLocation(double& outLat, double& outLon)
        {
            std::string moniker;
            return GetCurrentLocation(outLat, outLon, moniker);
        }

        // Overload that also reports which registered GeolocationInterface the
        // fix came from (e.g. "gps", "static") — useful for debugging which
        // source is actually driving the current position.
        static bool GetCurrentLocation(double& outLat, double& outLon, std::string& outMoniker)
        {
            if (!_PollingEnabled())
            {
                return _FetchFromSources(outLat, outLon, outMoniker);
            }

            bool ok = false;
            if (xSemaphoreTake(_CacheMutex(), portMAX_DELAY) == pdTRUE)
            {
                ok = _CacheValid() &&
                     (millis() - _CacheTimestampMs()) <= _LocationMaxAgeMs();
                if (ok)
                {
                    outLat = _CachedLat();
                    outLon = _CachedLon();
                    outMoniker = _CachedMoniker();
                }
                xSemaphoreGive(_CacheMutex());
            }

            if (ok)
            {
                ESP_LOGD(TAG, "Returning cached location from %s. Lat: %f, Lon: %f", outMoniker.c_str(), outLat, outLon);
            }
            else
            {
                ESP_LOGW(TAG, "No fresh cached location available");
            }
            return ok;
        }

        // Refreshes the location cache from the registered sources. Called by the
        // background polling task; on failure the previous cache is left intact
        // and the age check in GetCurrentLocation handles staleness.
        static void RefreshLocationCache()
        {
            double lat, lon;
            std::string moniker;
            if (!_FetchFromSources(lat, lon, moniker))
            {
                return;
            }

            if (xSemaphoreTake(_CacheMutex(), portMAX_DELAY) == pdTRUE)
            {
                _CachedLat() = lat;
                _CachedLon() = lon;
                _CachedMoniker() = moniker;
                _CacheTimestampMs() = millis();
                _CacheValid() = true;
                xSemaphoreGive(_CacheMutex());
            }
        }

        static void EnableLocationCache(bool enabled) { _PollingEnabled() = enabled; }
        static void SetLocationMaxAge(uint32_t ms) { _LocationMaxAgeMs() = ms; }
        static void SetPollIntervalMs(uint32_t ms) { _PollIntervalMs() = ms; }
        static uint32_t PollIntervalMs() { return _PollIntervalMs(); }

        static std::vector<GeolocationInterface*>& LocationSources()
        {
            static std::vector<GeolocationInterface*> sources;
            return sources;
        }

    private:
        // Iterates the registered location sources, returning the first success.
        // This is the original on-demand fetch behavior, extracted so both the
        // legacy path and the background poll task can reuse it.
        static bool _FetchFromSources(double& outLat, double& outLon, std::string& outMoniker)
        {
            for (auto* src : LocationSources())
            {
                if (!src->enabled)
                {
                    continue;
                }

                if (src->TryGetCurrentLocation(outLat, outLon))
                {
                    outMoniker = src->GetMoniker();
                    ESP_LOGD(TAG, "Location obtained from %s. Lat: %f, Lon: %f", outMoniker.c_str(), outLat, outLon);
                    return true;
                }
            }

            ESP_LOGW(TAG, "Failed to obtain location from all sources");
            return false;
        }

        static bool _LoadSourceEnabled(const std::string& moniker)
        {
            Preferences prefs;
            prefs.begin(GEO_SOURCE_PREFS_NAMESPACE, true);
            bool enabled = prefs.getBool(moniker.c_str(), true);
            prefs.end();
            return enabled;
        }

        static void _SaveSourceEnabled(const std::string& moniker, bool enabled)
        {
            Preferences prefs;
            prefs.begin(GEO_SOURCE_PREFS_NAMESPACE, false);
            prefs.putBool(moniker.c_str(), enabled);
            prefs.end();
        }

        static CompassInterface*& _Compass()
        {
            static CompassInterface* compass = nullptr;
            return compass;
        }

        // ---- Location cache state (used when background polling is enabled) ----
        static double& _CachedLat() { static double lat = 0.0; return lat; }
        static double& _CachedLon() { static double lon = 0.0; return lon; }
        static std::string& _CachedMoniker() { static std::string moniker; return moniker; }
        static uint32_t& _CacheTimestampMs() { static uint32_t ts = 0; return ts; }
        static bool& _CacheValid() { static bool valid = false; return valid; }
        static bool& _PollingEnabled() { static bool enabled = false; return enabled; }
        static uint32_t& _LocationMaxAgeMs() { static uint32_t ms = 60000; return ms; }
        static uint32_t& _PollIntervalMs() { static uint32_t ms = 15000; return ms; }

        static SemaphoreHandle_t& _CacheMutex()
        {
            static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
            return mutex;
        }

        // Note: was Stream& (references cannot be reseated — original Init() overload was UB).
        // Now Stream* with a default of &Serial2.
        static Stream*& _GpsInputStream()
        {
            static Stream* stream = &Serial2;
            return stream;
        }

        static TinyGPSLocation& _LastCoordinate()
        {
            static TinyGPSLocation coord;
            return coord;
        }

        static std::vector<SavedLocation>& _SavedLocations()
        {
            static std::vector<SavedLocation> locations;
            return locations;
        }
    };
}
