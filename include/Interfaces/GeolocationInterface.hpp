#pragma once
#include <Arduino.h>

namespace NavigationModule
{
    class GeolocationInterface
    {
    public:
        // Returns true and populates outLat/outLon (WGS84 decimal degrees) on success
        virtual bool TryGetCurrentLocation(double& outLat, double& outLon) = 0;
        // Short identifier for this source (e.g. "GpsSource"), used for debug logging
        virtual const char* GetMoniker() const = 0;
        virtual ~GeolocationInterface() = default;
    };
}
