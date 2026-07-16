#pragma once

#include <LittleFS.h>
#include <cstdint>
#include <cstring>

namespace NavigationModule
{
    /*
     * Compact BSSID -> lat/lon database, read from LittleFS.
     *
     * Ported from the prototype at
     * https://gist.github.com/ammaraskar/766eafcbb2d73d1920967c12621c4614 —
     * the on-disk format and lookup algorithm are unchanged; only the I/O was
     * swapped from stdio (fopen/fread/fseek) to the Arduino LittleFS File API
     * used everywhere else in this project (see FilesystemModule).
     *
     * -----------------------------------------------------------------------
     * On-disk format (version 1)
     * -----------------------------------------------------------------------
     *
     * All multi-byte integers are little-endian. Three sections, back to back:
     *
     *   +--------------------+ offset 0
     *   | Header (32 B)      |
     *   +--------------------+ offset 32
     *   | Bucket index       | (2^bucket_bits + 1) x uint32
     *   +--------------------+ records_off = 32 + (2^bucket_bits + 1) * 4
     *   | Records            | count x 10 B
     *   +--------------------+
     *
     * Header (32 bytes):
     *   offset size field        notes
     *   0      4    magic        "WGDB"
     *   4      1    version      1
     *   5      1    bucket_bits  buckets = 1 << bucket_bits (1..16)
     *   6      1    record_size  10
     *   7      1    reserved     0
     *   8      4    count        number of records
     *   12     4    lat_min      int32, degrees x 1e7 (quantization origin)
     *   16     4    lon_min      int32, degrees x 1e7
     *   20     4    scale        uint32, degrees x 1e7 per quantization step
     *   24     8    reserved     0
     *
     * Coordinate quantization (16-bit offsets from the per-file origin):
     *   encoded = round((deg * 1e7 - min_e7) / scale)   (clamped 0..65535)
     *   decoded = (min_e7 + encoded * scale) * 1e-7 degrees
     * At the default scale = 100 (~1.11 m) a 16-bit axis spans ~73 km; the
     * builder coarsens scale for larger bounding boxes.
     *
     * Bucket index: records are partitioned into 2^bucket_bits buckets by the
     * top bits of the FNV-1a hash of the raw 6-byte BSSID (hashing matters —
     * real BSSIDs cluster by vendor OUI). The index stores buckets + 1
     * cumulative counts, so bucket b occupies records [index[b], index[b+1]).
     *
     * Records (10 bytes): 6 B BSSID, 2 B quantized lat (uint16), 2 B quantized
     * lon (uint16), grouped by bucket in (hash, BSSID) order. The full BSSID is
     * kept — storing only a hash would introduce silent false positives.
     *
     * Lookup: hash -> bucket; one 8-byte read of index[bucket..bucket+1]; one
     * seek + chunked memcmp scan of the bucket's records.
     *
     * Access model: getWifiLocation() below is called only from the background
     * location-poll task (via WiFiGeolocator), so the single shared File handle
     * and its seek position are never touched concurrently.
     */
    class WifiGeoDb
    {
    public:
        // Default LittleFS path; the app may Open() a different one.
        static constexpr const char *DEFAULT_PATH = "/wifi_geo.db";

        WifiGeoDb() = default;
        ~WifiGeoDb() { Close(); }

        // Owns a File handle; copying would double-close it.
        WifiGeoDb(const WifiGeoDb &) = delete;
        WifiGeoDb &operator=(const WifiGeoDb &) = delete;

        bool Open(const char *path)
        {
            Close();

            if (!LittleFS.exists(path))
            {
                ESP_LOGW(TAG, "WiFi geo DB not found: %s", path);
                return false;
            }

            _file = LittleFS.open(path, FILE_READ);
            if (!_file)
            {
                ESP_LOGE(TAG, "Failed to open WiFi geo DB: %s", path);
                return false;
            }

            uint8_t h[HEADER_SIZE];
            if (_file.read(h, HEADER_SIZE) != HEADER_SIZE)
            {
                ESP_LOGE(TAG, "WiFi geo DB truncated header: %s", path);
                Close();
                return false;
            }

            if (memcmp(h, "WGDB", 4) != 0 || h[4] != VERSION || h[6] != RECORD_SIZE)
            {
                ESP_LOGE(TAG, "WiFi geo DB bad magic/version/record size: %s", path);
                Close();
                return false;
            }

            _bucketBits = h[5];
            if (_bucketBits < 1 || _bucketBits > 16)
            {
                ESP_LOGE(TAG, "WiFi geo DB bad bucket_bits %u", (unsigned)_bucketBits);
                Close();
                return false;
            }

            _count    = GetU32(h + 8);
            _latMinE7 = (int32_t)GetU32(h + 12);
            _lonMinE7 = (int32_t)GetU32(h + 16);
            _scaleE7  = GetU32(h + 20);
            if (_scaleE7 == 0)
            {
                ESP_LOGE(TAG, "WiFi geo DB zero scale");
                Close();
                return false;
            }

            _numBuckets = 1u << _bucketBits;
            _indexOff   = HEADER_SIZE;
            _recordsOff = HEADER_SIZE + (_numBuckets + 1) * 4;

            ESP_LOGI(TAG, "Opened WiFi geo DB %s: %u records, %u buckets",
                     path, (unsigned)_count, (unsigned)_numBuckets);
            return true;
        }

        void Close()
        {
            if (_file)
            {
                _file.close();
            }
        }

        bool IsOpen() const { return (bool)_file; }
        uint32_t Count() const { return _count; }

        // Look up a BSSID (6 bytes, transmit order). Returns false when absent.
        bool Lookup(const uint8_t *bssid, double &lat, double &lng)
        {
            if (!_file || _count == 0)
            {
                return false;
            }

            uint32_t bucket = BucketOf(HashBssid(bssid), _bucketBits);

            uint8_t buf[8];
            if (!_file.seek(_indexOff + bucket * 4) || _file.read(buf, 8) != 8)
            {
                return false;
            }

            uint32_t start = GetU32(buf);
            uint32_t end   = GetU32(buf + 4);
            if (end <= start || end > _count)
            {
                return false; // empty bucket (or corrupt index)
            }

            if (!_file.seek(_recordsOff + start * RECORD_SIZE))
            {
                return false;
            }

            // Scanned in chunks to bound stack usage.
            static constexpr uint32_t SCAN_CHUNK = 16;
            uint8_t  recs[SCAN_CHUNK * RECORD_SIZE];
            uint32_t remaining = end - start;
            while (remaining > 0)
            {
                uint32_t chunk = remaining < SCAN_CHUNK ? remaining : SCAN_CHUNK;
                if (_file.read(recs, chunk * RECORD_SIZE) != chunk * RECORD_SIZE)
                {
                    return false;
                }

                for (uint32_t i = 0; i < chunk; i++)
                {
                    const uint8_t *r = recs + (size_t)i * RECORD_SIZE;
                    if (memcmp(r, bssid, 6) == 0)
                    {
                        lat = (_latMinE7 + (int64_t)GetU16(r + 6) * _scaleE7) * 1e-7;
                        lng = (_lonMinE7 + (int64_t)GetU16(r + 8) * _scaleE7) * 1e-7;
                        return true;
                    }
                }
                remaining -= chunk;
            }
            return false;
        }

    private:
        static constexpr const char *TAG         = "WifiGeoDb";
        static constexpr uint32_t    HEADER_SIZE = 32;
        static constexpr uint32_t    RECORD_SIZE = 10; // 6B bssid + 2B lat + 2B lon
        static constexpr uint8_t     VERSION     = 1;

        File     _file;
        uint32_t _count      = 0;
        uint8_t  _bucketBits = 0;
        int32_t  _latMinE7   = 0;
        int32_t  _lonMinE7   = 0;
        uint32_t _scaleE7    = 0;
        uint32_t _numBuckets = 0;
        uint32_t _indexOff   = 0;
        uint32_t _recordsOff = 0;

        // ---------- little-endian helpers (format is LE on any host) ----------

        static uint16_t GetU16(const uint8_t *p)
        {
            return (uint16_t)(p[0] | (p[1] << 8));
        }

        static uint32_t GetU32(const uint8_t *p)
        {
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }

        // ---------- hashing ----------

        // FNV-1a over the 6 BSSID bytes; scrambles clustered OUI prefixes well.
        static uint32_t HashBssid(const uint8_t bssid[6])
        {
            uint32_t h = 2166136261u;
            for (int i = 0; i < 6; i++)
            {
                h ^= bssid[i];
                h *= 16777619u;
            }
            return h;
        }

        static uint32_t BucketOf(uint32_t hash, uint8_t bits)
        {
            return hash >> (32 - bits); // bits is validated to be 1..16
        }
    };

    // Process-wide database instance (Meyers singleton). Open it once at app
    // init with the desired path, e.g.:
    //   NavigationModule::WifiGeoDbInstance().Open("/wifi_geo.db");
    inline WifiGeoDb &WifiGeoDbInstance()
    {
        static WifiGeoDb db;
        return db;
    }

    // Convenience for app init; returns false if the DB can't be opened.
    inline bool OpenWifiGeoDb(const char *path = WifiGeoDb::DEFAULT_PATH)
    {
        return WifiGeoDbInstance().Open(path);
    }

    // Interface WiFiGeolocator depends on. bssid is the 6-byte BSSID as
    // returned by WiFi.BSSID(i); lat/lng are filled when it is known.
    //
    // If the app hasn't opened the DB explicitly, this lazily opens the default
    // path once. The one-shot latch keeps a missing DB from re-attempting the
    // open on every scanned AP — call OpenWifiGeoDb() at init to control the
    // path and get a clear success/failure log at boot.
    inline bool getWifiLocation(uint8_t *bssid, double &lat, double &lng)
    {
        WifiGeoDb &db = WifiGeoDbInstance();
        if (!db.IsOpen())
        {
            static bool attempted = false;
            if (attempted)
            {
                return false;
            }
            attempted = true;
            if (!db.Open(WifiGeoDb::DEFAULT_PATH))
            {
                return false;
            }
        }
        return db.Lookup(bssid, lat, lng);
    }
}
