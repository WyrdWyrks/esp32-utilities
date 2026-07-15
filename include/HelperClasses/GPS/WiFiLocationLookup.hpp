#pragma once

#include <cstdint>
#include <unordered_map>

namespace NavigationModule
{
    // ================================ STUB ================================
    // Placeholder for the LittleFS-backed BSSID -> coordinate lookup being
    // developed separately (eventually reading a known-AP table from flash via
    // FilesystemModule). Replace the body below with the real lookup; keep the
    // signature — WiFiGeolocator depends on it.
    //
    //   bssid : pointer to the 6-byte BSSID in transmit order, as returned by
    //           WiFi.BSSID(i).
    //   lat/lng : filled with the AP's coordinates when it is known.
    //   returns : true if the BSSID was found, false otherwise.
    // =====================================================================
    inline bool getWifiLocation(uint8_t* bssid, double& lat, double& lng)
    {
        struct Coord
        {
            double lat;
            double lng;
        };

        // Hardcoded sample table until the LittleFS lookup lands. The key is the
        // 6-byte MAC packed into the low 48 bits of a uint64_t — just the MAC
        // hex with the colons removed: AA:BB:CC:DD:EE:FF -> 0xAABBCCDDEEFFULL.
        static const std::unordered_map<uint64_t, Coord> table = {
            { 0x0ULL, { 0.0, 0.0 } }
        };

        uint64_t key = 0;
        for (int i = 0; i < 6; ++i)
        {
            key = (key << 8) | bssid[i];
        }

        auto it = table.find(key);
        if (it == table.end())
        {
            return false;
        }

        lat = it->second.lat;
        lng = it->second.lng;
        return true;
    }
}
