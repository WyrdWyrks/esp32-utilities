#pragma once
#include <Arduino.h>

namespace NavigationModule
{
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
    };
}
