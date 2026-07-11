#pragma once

#include "GeolocationInterface.hpp"

namespace NavigationModule
{
    class StaticLocation : public GeolocationInterface
    {
    public:
        StaticLocation(double lat, double lon) : _lat(lat), _lon(lon) {}

        bool TryGetCurrentLocation(double& outLat, double& outLon) override
        {
            outLat = _lat;
            outLon = _lon;
            return true;
        }

        const char* GetMoniker() const override { return "static"; }

        // A constant location is never actually "stale" — report a freshly
        // "polled" result on every call instead of relying on the cached/
        // _PublishResult path (which would age out between real polls).
        GeolocationResult GetLastResult() const override
        {
            GeolocationResult result;
            result.everPublished = true;
            result.hasFix        = true;
            result.lat            = _lat;
            result.lon            = _lon;
            result.moniker        = GetMoniker();
            result.timestampMs    = millis();
            return result;
        }

    private:
        double _lat;
        double _lon;
    };
}
