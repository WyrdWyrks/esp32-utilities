#pragma once
#include <Arduino.h>
#include <string>

namespace NavigationModule
{
    // Snapshot of the last result a GeolocationInterface produced. Read via
    // GetLastResult() — never triggers hardware/network I/O, so it's safe to
    // call from any task, including the UI/render thread.
    struct GeolocationResult
    {
        bool     everPublished = false; // false until TryGetCurrentLocation has run at least once
        bool     hasFix        = false;
        double   lat           = 0.0;
        double   lon           = 0.0;
        std::string moniker;
        uint32_t timestampMs   = 0;     // millis() when this was captured

        uint32_t AgeMs() const { return millis() - timestampMs; }
    };

    class GeolocationInterface
    {
    public:
        // Returns true and populates outLat/outLon (WGS84 decimal degrees) on success
        virtual bool TryGetCurrentLocation(double& outLat, double& outLon) = 0;
        // Short identifier for this source (e.g. "gps"), used for debug logging
        // and as the Preferences key when persisting the enabled state below
        virtual const char* GetMoniker() const = 0;
        virtual ~GeolocationInterface() = default;

        // Whether this source should be queried. Sources start enabled by
        // default; NavigationModule::Utilities::RegisterLocationSource loads
        // the persisted value (keyed by GetMoniker()) over this default.
        bool enabled = true;

        // Last result this source produced, populated by _PublishResult().
        // Default implementation returns a mutex-guarded copy of the cache;
        // override when a source has no real notion of staleness (see
        // StaticLocation, which always reports itself as freshly polled).
        virtual GeolocationResult GetLastResult() const
        {
            GeolocationResult copy;
            if (xSemaphoreTake(_resultMutex, portMAX_DELAY) == pdTRUE)
            {
                copy = _lastResult;
                xSemaphoreGive(_resultMutex);
            }
            return copy;
        }

    protected:
        // Derived classes call this right before returning from
        // TryGetCurrentLocation to publish the result for GetLastResult().
        void _PublishResult(bool hasFix, double lat, double lon)
        {
            if (xSemaphoreTake(_resultMutex, portMAX_DELAY) == pdTRUE)
            {
                _lastResult.everPublished = true;
                _lastResult.hasFix        = hasFix;
                if (hasFix)
                {
                    _lastResult.lat = lat;
                    _lastResult.lon = lon;
                }
                _lastResult.moniker     = GetMoniker();
                _lastResult.timestampMs = millis();
                xSemaphoreGive(_resultMutex);
            }
        }

    private:
        GeolocationResult _lastResult;
        SemaphoreHandle_t _resultMutex = xSemaphoreCreateMutex();
    };
}
