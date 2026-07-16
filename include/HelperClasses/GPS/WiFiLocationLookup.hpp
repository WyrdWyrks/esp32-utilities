#pragma once

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "SystemUtilities.hpp"

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
     * Access model: getWifiLocation() is called from the background
     * location-poll task (via WiFiGeolocator) while the RPC upload handlers
     * (RpcClearWifiGeoDb, RpcInsertWifiGeoDbBlock) run on the RPC task. Open,
     * Close, Lookup, Clear and AppendBlock all take an internal mutex so those
     * two tasks can't corrupt the shared File handle mid-operation.
     */
    class WifiGeoDb
    {
    public:
        // Default LittleFS path; the app may Open() a different one.
        static constexpr const char *DEFAULT_PATH = "/wifi_geo.db";

        // Outcome of AppendBlock — the RPC handler needs to distinguish
        // open failure from an offset mismatch from a short write to give
        // the caller a useful error.
        struct AppendOutcome
        {
            bool     openFailed     = false;
            bool     offsetMismatch = false;
            uint32_t sizeBefore     = 0; // file size just before the write
            size_t   written        = 0; // bytes actually written this call
        };

        WifiGeoDb() = default;
        ~WifiGeoDb() { Close(); }

        // Owns a File handle; copying would double-close it.
        WifiGeoDb(const WifiGeoDb &) = delete;
        WifiGeoDb &operator=(const WifiGeoDb &) = delete;

        bool Open(const char *path)
        {
            Lock lock(_mutex);
            return _OpenUnlocked(path);
        }

        void Close()
        {
            Lock lock(_mutex);
            _CloseUnlocked();
        }

        // Single-word reads — safe to skip the lock. Callers that need an
        // atomic (IsOpen && Lookup) pair should just call Lookup, which
        // re-checks under the lock.
        bool     IsOpen() const { return (bool)_file; }
        uint32_t Count() const  { return _count; }

        // Look up a BSSID (6 bytes, transmit order). Returns false when absent.
        bool Lookup(const uint8_t *bssid, double &lat, double &lng)
        {
            Lock lock(_mutex);

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

        // Close the read handle and remove the file. Returns true if the file
        // was gone (or never existed) afterwards.
        bool Clear(const char *path)
        {
            Lock lock(_mutex);
            _CloseUnlocked();
            if (!LittleFS.exists(path))
            {
                return true;
            }
            return LittleFS.remove(path);
        }

        // Close the read handle and append data to the file (created if
        // missing). If expectedOffset is non-null the current file size must
        // match it before writing, otherwise the write is skipped and
        // offsetMismatch is set. Never re-opens the read handle — the caller
        // is expected to trigger reopen once the upload is complete.
        AppendOutcome AppendBlock(const char    *path,
                                  const uint8_t *data,
                                  size_t         len,
                                  const uint32_t *expectedOffset)
        {
            Lock lock(_mutex);
            AppendOutcome out;
            _CloseUnlocked();

            File f = LittleFS.open(path, FILE_APPEND);
            if (!f)
            {
                out.openFailed = true;
                return out;
            }

            out.sizeBefore = (uint32_t)f.size();
            if (expectedOffset && *expectedOffset != out.sizeBefore)
            {
                out.offsetMismatch = true;
                f.close();
                return out;
            }

            out.written = f.write(data, len);
            f.close();
            return out;
        }

    private:
        static constexpr const char *TAG         = "WifiGeoDb";
        static constexpr uint32_t    HEADER_SIZE = 32;
        static constexpr uint32_t    RECORD_SIZE = 10; // 6B bssid + 2B lat + 2B lon
        static constexpr uint8_t     VERSION     = 1;

        // RAII wrapper around the FreeRTOS mutex — mirrors the pattern used
        // in GeolocationInterface::GetLastResult, but automated so the many
        // early returns in Lookup can't leak the lock.
        struct Lock
        {
            SemaphoreHandle_t &s;
            Lock(SemaphoreHandle_t &sem) : s(sem) { xSemaphoreTake(s, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(s); }
            Lock(const Lock &)            = delete;
            Lock &operator=(const Lock &) = delete;
        };

        bool _OpenUnlocked(const char *path)
        {
            _CloseUnlocked();

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
                _CloseUnlocked();
                return false;
            }

            if (memcmp(h, "WGDB", 4) != 0 || h[4] != VERSION || h[6] != RECORD_SIZE)
            {
                ESP_LOGE(TAG, "WiFi geo DB bad magic/version/record size: %s", path);
                _CloseUnlocked();
                return false;
            }

            _bucketBits = h[5];
            if (_bucketBits < 1 || _bucketBits > 16)
            {
                ESP_LOGE(TAG, "WiFi geo DB bad bucket_bits %u", (unsigned)_bucketBits);
                _CloseUnlocked();
                return false;
            }

            _count    = GetU32(h + 8);
            _latMinE7 = (int32_t)GetU32(h + 12);
            _lonMinE7 = (int32_t)GetU32(h + 16);
            _scaleE7  = GetU32(h + 20);
            if (_scaleE7 == 0)
            {
                ESP_LOGE(TAG, "WiFi geo DB zero scale");
                _CloseUnlocked();
                return false;
            }

            _numBuckets = 1u << _bucketBits;
            _indexOff   = HEADER_SIZE;
            _recordsOff = HEADER_SIZE + (_numBuckets + 1) * 4;

            ESP_LOGI(TAG, "Opened WiFi geo DB %s: %u records, %u buckets",
                     path, (unsigned)_count, (unsigned)_numBuckets);
            return true;
        }

        void _CloseUnlocked()
        {
            if (_file)
            {
                _file.close();
            }
        }

        SemaphoreHandle_t _mutex      = xSemaphoreCreateMutex();
        File              _file;
        uint32_t          _count      = 0;
        uint8_t           _bucketBits = 0;
        int32_t           _latMinE7   = 0;
        int32_t           _lonMinE7   = 0;
        uint32_t          _scaleE7    = 0;
        uint32_t          _numBuckets = 0;
        uint32_t          _indexOff   = 0;
        uint32_t          _recordsOff = 0;

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

    // Latch that keeps getWifiLocation() from re-attempting the lazy open on
    // every scanned AP when the DB file is missing. Exposed as a Meyers
    // singleton so the RPC handlers below can clear it after a clear/insert —
    // otherwise the DB would stay unreachable until reboot after a rewrite.
    inline bool &_WifiGeoDbAutoOpenLatch()
    {
        static bool attempted = false;
        return attempted;
    }

    // Interface WiFiGeolocator depends on. bssid is the 6-byte BSSID as
    // returned by WiFi.BSSID(i); lat/lng are filled when it is known.
    //
    // If the app hasn't opened the DB explicitly, this lazily opens the default
    // path once. The latch keeps a missing DB from re-attempting the open on
    // every scanned AP — call OpenWifiGeoDb() at init to control the path and
    // get a clear success/failure log at boot.
    inline bool getWifiLocation(uint8_t *bssid, double &lat, double &lng)
    {
        WifiGeoDb &db = WifiGeoDbInstance();
        if (!db.IsOpen())
        {
            if (_WifiGeoDbAutoOpenLatch())
            {
                return false;
            }
            _WifiGeoDbAutoOpenLatch() = true;
            if (!db.Open(WifiGeoDb::DEFAULT_PATH))
            {
                return false;
            }
        }
        return db.Lookup(bssid, lat, lng);
    }

    // -----------------------------------------------------------------------
    // RPC handlers — the application layer uploads a new DB by clearing the
    // existing file and streaming its bytes back in base64-encoded chunks.
    // Matches the shape of the OTA RPC in SystemUtilities.hpp so the PWA can
    // reuse the same encode + chunk + checksum path. Register with:
    //   RpcModule::Utilities::RegisterRpc("ClearWifiGeoDb",
    //                                     NavigationModule::RpcClearWifiGeoDb);
    //   RpcModule::Utilities::RegisterRpc("InsertWifiGeoDbBlock",
    //                                     NavigationModule::RpcInsertWifiGeoDbBlock);
    //
    // Payloads (lowercase keys, matching UploadOtaChunkRpc):
    //   ClearWifiGeoDb        { "path"?: string }
    //     -> { "status": "cleared" }  |  { "error": string }
    //
    //   InsertWifiGeoDbBlock  { "chunk": base64-string,
    //                           "checksum": uint32,        // sum of raw bytes
    //                           "offset"?: uint32,         // optional guard
    //                           "path"?: string }
    //     -> { "written": uint, "total_size": uint }
    //     |  { "error": string, "expected_offset"?: uint }
    //
    // "chunk" carries raw DB bytes (header, index, or records — the ESP32 is a
    // byte pipe; the PWA is responsible for producing a valid on-disk layout,
    // see the format comment at the top of this file). Blocks are appended in
    // order; if "offset" is supplied it is checked against the current file
    // size so the caller can detect a lost/reordered chunk. On mismatch the
    // real current size is returned in "expected_offset" and the caller should
    // Clear + restart.
    //
    // Both handlers rely on WifiGeoDb's internal mutex to serialize against
    // the poll task's Lookup calls, and clear the auto-open latch so the next
    // getWifiLocation() call reopens the freshly written DB.
    // -----------------------------------------------------------------------

    inline void RpcClearWifiGeoDb(JsonDocument &doc)
    {
        const char *path = doc["path"].isNull()
                               ? WifiGeoDb::DEFAULT_PATH
                               : doc["path"].as<const char *>();

        bool ok = WifiGeoDbInstance().Clear(path);
        _WifiGeoDbAutoOpenLatch() = false;

        doc.clear();
        if (ok)
        {
            doc["status"] = "cleared";
        }
        else
        {
            doc["error"] = "remove failed";
        }
    }

    inline void RpcInsertWifiGeoDbBlock(JsonDocument &doc)
    {
        if (doc["chunk"].isNull())
        {
            doc.clear();
            doc["error"] = "Missing or invalid 'chunk'";
            return;
        }
        auto b64 = doc["chunk"].as<std::string>();

        if (doc["checksum"].isNull())
        {
            doc.clear();
            doc["error"] = "Missing or invalid 'checksum'";
            return;
        }
        auto checksum = doc["checksum"].as<uint32_t>();

        bool     hasOffset      = !doc["offset"].isNull();
        uint32_t expectedOffset = hasOffset ? doc["offset"].as<uint32_t>() : 0;

        const char *path = doc["path"].isNull()
                               ? WifiGeoDb::DEFAULT_PATH
                               : doc["path"].as<const char *>();

        size_t b64Len   = b64.size();
        size_t binLen   = (b64Len * 3) / 4; // upper bound; DecodeBase64 returns actual
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[binLen]);

        int actualLen = System_Utils::DecodeBase64(b64.c_str(), buffer.get(), binLen);
        if (actualLen <= 0)
        {
            doc.clear();
            doc["error"] = "Base64 decode failed";
            return;
        }

        uint32_t calculatedChecksum = 0;
        for (int i = 0; i < actualLen; i++)
        {
            calculatedChecksum += buffer[i];
        }
        if (calculatedChecksum != checksum)
        {
            doc.clear();
            doc["error"] = "CRC mismatch";
            return;
        }

        auto outcome = WifiGeoDbInstance().AppendBlock(
            path, buffer.get(), (size_t)actualLen,
            hasOffset ? &expectedOffset : nullptr);

        _WifiGeoDbAutoOpenLatch() = false;

        doc.clear();
        if (outcome.openFailed)
        {
            doc["error"] = "open failed";
        }
        else if (outcome.offsetMismatch)
        {
            doc["error"]           = "offset mismatch";
            doc["expected_offset"] = outcome.sizeBefore;
        }
        else if (outcome.written != (size_t)actualLen)
        {
            doc["error"]   = "short write";
            doc["written"] = (uint32_t)outcome.written;
        }
        else
        {
            doc["written"]    = (uint32_t)outcome.written;
            doc["total_size"] = outcome.sizeBefore + (uint32_t)outcome.written;
        }
    }
}
