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

    private:
        double _lat;
        double _lon;
    };
}
