#pragma once

#include "GeolocationInterface.hpp"
#include "WiFiLocationLookup.hpp"
#include <WiFi.h>
#include <cmath>

namespace NavigationModule
{
    // Estimates position by scanning nearby WiFi access points and looking up
    // each one's surveyed coordinates via WifiGeoDb::getWifiLocation() (a
    // LittleFS-backed BSSID -> lat/lon database). Every scanned AP found in
    // that database contributes its coordinates to a signal-strength-
    // weighted centroid: a stronger RSSI implies the AP is closer, so it gets
    // more weight and pulls the estimate toward itself. The result is coarse
    // (tens to hundreds of metres) but works indoors and in urban canyons
    // where GPS struggles.
    //
    // RSSI is turned into a distance with the log-distance path-loss model
    // (inspired by jvillagomez/rssi_module) and combined with weighted-centroid
    // positioning (inspired by futurice/whereareyou). Both the reference RSSI
    // and the path-loss exponent are environment dependent and exposed as
    // tunables below; the defaults are reasonable indoor starting points.
    //
    // WiFi must already be initialised (STA or STA+AP) for the scan to succeed;
    // this class never changes the radio mode itself, to avoid disturbing the
    // connectivity module. On ESP32 an active scan briefly interrupts an STA
    // connection, which then reconnects automatically.
    //
    // Called only from the background location-poll task (see
    // NavigationModule::Utilities), so the blocking scan is safe here.
    class WiFiGeolocator : public GeolocationInterface
    {
    public:
        WiFiGeolocator() = default;

        // ------------------------------------------------------------------
        // Tuning
        // ------------------------------------------------------------------

        // Reference RSSI measured 1 m from an AP (dBm). Larger (closer to 0)
        // shifts every distance estimate outward.
        void SetReferenceRssi(int rssiAtOneMeter) { _rssiAtOneMeter = rssiAtOneMeter; }
        // Path-loss exponent n: ~2 in free space, ~3-4 through walls.
        void SetPathLossExponent(double n) { _pathLossExponent = n; }
        // Exponent applied to 1/distance when weighting (2 == inverse-square).
        void SetWeightExponent(double g) { _weightExponent = g; }
        // Scanned APs weaker than this (dBm) are ignored as unreliable.
        void SetMinRssi(int minRssi) { _minRssi = minRssi; }
        // Require at least this many matched APs before reporting a fix.
        void SetMinMatches(size_t n) { _minMatches = n; }

        // ------------------------------------------------------------------
        // GeolocationInterface
        // ------------------------------------------------------------------

        bool TryGetCurrentLocation(double& outLat, double& outLon) override
        {
            int16_t n = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/);
            if (n < 0)
            {
                ESP_LOGW(_TAG, "WiFi scan failed (code %d); is WiFi initialised?", n);
                _PublishResult(false, 0, 0);
                return false;
            }

            ESP_LOGD(_TAG, "Scanned %d WiFi network(s)", n);

            double sumWeight  = 0.0;
            double sumLat     = 0.0;
            double sumLon     = 0.0;
            size_t matchCount = 0;

            for (int16_t i = 0; i < n; ++i)
            {
                int rssi = (int)WiFi.RSSI(i);
                if (rssi < _minRssi)
                {
                    continue;
                }

                // WiFi.BSSID(i) points into the driver's scan buffer, valid
                // until scanDelete() below.
                double apLat = 0.0;
                double apLon = 0.0;
                if (!WifiGeoDb::getWifiLocation(WiFi.BSSID(i), apLat, apLon))
                {
                    continue; // unknown AP
                }

                double distance = _EstimateDistanceMeters(rssi);
                double weight    = 1.0 / pow(distance, _weightExponent);

                sumWeight += weight;
                sumLat    += weight * apLat;
                sumLon    += weight * apLon;
                matchCount++;

                ESP_LOGD(_TAG, "Matched %s rssi=%d dist=%.1fm w=%.4f -> (%.6f, %.6f)",
                         WiFi.BSSIDstr(i).c_str(), rssi, distance, weight, apLat, apLon);
            }

            WiFi.scanDelete(); // free the driver-side scan buffer

            if (matchCount < _minMatches || sumWeight <= 0.0)
            {
                ESP_LOGV(_TAG, "Matched %u known AP(s), need %u",
                         (unsigned)matchCount, (unsigned)_minMatches);
                _PublishResult(false, 0, 0);
                return false;
            }

            outLat = sumLat / sumWeight;
            outLon = sumLon / sumWeight;

            ESP_LOGD(_TAG, "WiFi fix from %u AP(s): %.6f, %.6f",
                     (unsigned)matchCount, outLat, outLon);
            _PublishResult(true, outLat, outLon);
            return true;
        }

        const char* GetMoniker() const override { return "wifi"; }

    private:
        // Log-distance path-loss model: d = 10^((refRssi - rssi) / (10 * n)).
        // At rssi == refRssi this yields 1 m; weaker signals yield larger d.
        double _EstimateDistanceMeters(int rssi) const
        {
            return pow(10.0, (double)(_rssiAtOneMeter - rssi) / (10.0 * _pathLossExponent));
        }

        int    _rssiAtOneMeter   = -45;  // dBm at 1 m (reference)
        double _pathLossExponent = 3.0;  // indoor-ish attenuation
        double _weightExponent   = 2.0;  // inverse-square distance weighting
        int    _minRssi          = -90;  // ignore APs weaker than this
        size_t _minMatches       = 1;    // matched APs required for a fix

        const char* _TAG = "WiFiGeolocator";
    };
}
