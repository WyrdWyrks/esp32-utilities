#pragma once

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstdint>
#include <cstring>
#include <memory>

#include "RpcUtils.h"
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
     * (RpcClear, RpcInsertBlock) run on the RPC task.
     * Open, Close, Lookup, Clear and AppendBlock all take an internal mutex so
     * those two tasks can't corrupt the shared File handle mid-operation.
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

        // Static-only: there is never more than one WiFi geo DB on the device,
        // so the state lives in a single Meyers singleton (_DbState()) instead of an
        // instance. Not constructible.
        WifiGeoDb() = delete;

        static bool Open(const char *path = DEFAULT_PATH)
        {
            Lock lock(_DbState().mutex);
            return _OpenUnlocked(path);
        }

        static void Close()
        {
            Lock lock(_DbState().mutex);
            _CloseUnlocked();
        }

        // Single-word reads — safe to skip the lock. Callers that need an
        // atomic (IsOpen && Lookup) pair should just call Lookup, which
        // re-checks under the lock.
        static bool     IsOpen()     { return (bool)_DbState().file; }
        static uint32_t Count()      { return _DbState().count; }
        static uint8_t  BucketBits() { return _DbState().bucketBits; }

        // Look up a BSSID (6 bytes, transmit order). Returns false when absent.
        static bool Lookup(const uint8_t *bssid, double &lat, double &lng)
        {
            DbState &s = _DbState();
            Lock lock(s.mutex);

            if (!s.file || s.count == 0)
            {
                return false;
            }

            uint32_t bucket = BucketOf(HashBssid(bssid), s.bucketBits);

            uint8_t buf[8];
            if (!s.file.seek(s.indexOff + bucket * 4) || s.file.read(buf, 8) != 8)
            {
                return false;
            }

            uint32_t start = GetU32(buf);
            uint32_t end   = GetU32(buf + 4);
            if (end <= start || end > s.count)
            {
                return false; // empty bucket (or corrupt index)
            }

            if (!s.file.seek(s.recordsOff + start * RECORD_SIZE))
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
                if (s.file.read(recs, chunk * RECORD_SIZE) != chunk * RECORD_SIZE)
                {
                    return false;
                }

                for (uint32_t i = 0; i < chunk; i++)
                {
                    const uint8_t *r = recs + (size_t)i * RECORD_SIZE;
                    if (memcmp(r, bssid, 6) == 0)
                    {
                        lat = (s.latMinE7 + (int64_t)GetU16(r + 6) * s.scaleE7) * 1e-7;
                        lng = (s.lonMinE7 + (int64_t)GetU16(r + 8) * s.scaleE7) * 1e-7;
                        return true;
                    }
                }
                remaining -= chunk;
            }
            return false;
        }

        // Entry point WiFiGeolocator calls, once per scanned AP. bssid is the
        // 6-byte BSSID as returned by WiFi.BSSID(i); lat/lng are filled when it
        // is known. Lazily opens the default DB once if the app hasn't opened
        // one — running once per AP, a missing DB must not re-attempt the open
        // every call, so the latch makes it one-shot. Prefer calling Open() at
        // init to control the path and get a clear boot-time log.
        static bool getWifiLocation(uint8_t *bssid, double &lat, double &lng)
        {
            if (!IsOpen())
            {
                if (_DbState().autoOpenAttempted)
                {
                    return false;
                }
                _DbState().autoOpenAttempted = true;
                if (!Open(DEFAULT_PATH))
                {
                    return false;
                }
            }
            return Lookup(bssid, lat, lng);
        }

        // Clears the one-shot latch above so the next LookupAutoOpen() reopens
        // the file. The RPC handlers call this after a clear/insert, otherwise a
        // freshly written DB would stay unreachable until reboot.
        static void ResetAutoOpenLatch() { _DbState().autoOpenAttempted = false; }

        // Close the read handle and remove the file. Returns true if the file
        // was gone (or never existed) afterwards.
        static bool Clear(const char *path = DEFAULT_PATH)
        {
            Lock lock(_DbState().mutex);
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
        static AppendOutcome AppendBlock(const char     *path,
                                         const uint8_t  *data,
                                         size_t          len,
                                         const uint32_t *expectedOffset)
        {
            Lock lock(_DbState().mutex);
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

        // -------------------------------------------------------------------
        // RPC — the application layer uploads a new DB by clearing the existing
        // file and streaming its bytes back in base64-encoded chunks. Matches
        // the shape of the OTA RPC in SystemUtilities.hpp so the PWA can reuse
        // the same encode + chunk + checksum path. Register both with one call
        // from app init:
        //   NavigationModule::WifiGeoDb::RegisterRpcs();
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
        //   GetWifiGeoDbInfo      { "path"?: string }
        //     -> { "open": bool, "count": uint32, "bucket_bits": uint8 }
        //   Opens the DB (if not already open) to read its header, so this
        //   also works as a "does a valid DB exist" check right after boot,
        //   before anything else has triggered an auto-open.
        //
        // "chunk" carries raw DB bytes (header, index, or records — the ESP32 is
        // a byte pipe; the PWA is responsible for producing a valid on-disk
        // layout, see the format comment at the top of this file). Blocks are
        // appended in order; if "offset" is supplied it is checked against the
        // current file size so the caller can detect a lost/reordered chunk. On
        // mismatch the real current size is returned in "expected_offset" and
        // the caller should clear + restart.
        //
        // Both handlers route through Clear/AppendBlock above, so they take the
        // same mutex as the poll task's lookups, and both reset the auto-open
        // latch so the next getWifiLocation() reopens the freshly written DB.
        //
        // The Rpc prefix is not just convention here: Clear(const char*) and
        // AppendBlock(...) already exist, so same-named JsonDocument overloads
        // would make &WifiGeoDb::Clear ambiguous when passed to RegisterRpc.
        // -------------------------------------------------------------------

        static constexpr const char *CLEAR_RPC_NAME    = "ClearWifiGeoDb";
        static constexpr const char *INSERT_RPC_NAME   = "InsertWifiGeoDbBlock";
        static constexpr const char *GET_INFO_RPC_NAME = "GetWifiGeoDbInfo";

        // Registers every WiFi geo DB RPC in one call. Safe to call again —
        // RegisterRpc overwrites by name.
        static void RegisterRpcs()
        {
            RpcModule::Utilities::RegisterRpc(CLEAR_RPC_NAME, &WifiGeoDb::RpcClear);
            RpcModule::Utilities::RegisterRpc(INSERT_RPC_NAME, &WifiGeoDb::RpcInsertBlock);
            RpcModule::Utilities::RegisterRpc(GET_INFO_RPC_NAME, &WifiGeoDb::RpcGetInfo);
            ESP_LOGI(TAG, "Registered %s, %s, and %s", CLEAR_RPC_NAME, INSERT_RPC_NAME, GET_INFO_RPC_NAME);
        }

        static void RpcClear(JsonDocument &doc)
        {
            const char *path = doc["path"].isNull()
                                   ? DEFAULT_PATH
                                   : doc["path"].as<const char *>();

            bool ok = Clear(path);
            ResetAutoOpenLatch();

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

        static void RpcInsertBlock(JsonDocument &doc)
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
                                   ? DEFAULT_PATH
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

            auto outcome = AppendBlock(path, buffer.get(), (size_t)actualLen,
                                       hasOffset ? &expectedOffset : nullptr);

            ResetAutoOpenLatch();

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

        // Opens the DB (if not already open) so this reports the real
        // on-disk state rather than just whatever happened to be cached —
        // in particular it works as a boot-time "is there a valid DB"
        // check before the location poll task has attempted its own
        // lazy auto-open.
        static void RpcGetInfo(JsonDocument &doc)
        {
            const char *path = doc["path"].isNull()
                                   ? DEFAULT_PATH
                                   : doc["path"].as<const char *>();

            if (!IsOpen())
            {
                Open(path);
            }

            doc.clear();
            doc["open"]        = IsOpen();
            doc["count"]       = Count();
            doc["bucket_bits"] = BucketBits();
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

        // All DB state. One Meyers singleton holding a struct, rather than an
        // accessor per field — these are private internals and there is only
        // ever one DB, so the extra indirection would buy nothing.
        struct DbState
        {
            SemaphoreHandle_t mutex             = xSemaphoreCreateMutex();
            File              file;
            uint32_t          count             = 0;
            uint8_t           bucketBits        = 0;
            int32_t           latMinE7          = 0;
            int32_t           lonMinE7          = 0;
            uint32_t          scaleE7           = 0;
            uint32_t          numBuckets        = 0;
            uint32_t          indexOff          = 0;
            uint32_t          recordsOff        = 0;
            bool              autoOpenAttempted = false;
        };

        static DbState &_DbState()
        {
            static DbState s;
            return s;
        }

        static bool _OpenUnlocked(const char *path)
        {
            DbState &s = _DbState();
            _CloseUnlocked();

            if (!LittleFS.exists(path))
            {
                ESP_LOGW(TAG, "WiFi geo DB not found: %s", path);
                return false;
            }

            s.file = LittleFS.open(path, FILE_READ);
            if (!s.file)
            {
                ESP_LOGE(TAG, "Failed to open WiFi geo DB: %s", path);
                return false;
            }

            uint8_t h[HEADER_SIZE];
            if (s.file.read(h, HEADER_SIZE) != HEADER_SIZE)
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

            s.bucketBits = h[5];
            if (s.bucketBits < 1 || s.bucketBits > 16)
            {
                ESP_LOGE(TAG, "WiFi geo DB bad bucket_bits %u", (unsigned)s.bucketBits);
                _CloseUnlocked();
                return false;
            }

            s.count    = GetU32(h + 8);
            s.latMinE7 = (int32_t)GetU32(h + 12);
            s.lonMinE7 = (int32_t)GetU32(h + 16);
            s.scaleE7  = GetU32(h + 20);
            if (s.scaleE7 == 0)
            {
                ESP_LOGE(TAG, "WiFi geo DB zero scale");
                _CloseUnlocked();
                return false;
            }

            s.numBuckets = 1u << s.bucketBits;
            s.indexOff   = HEADER_SIZE;
            s.recordsOff = HEADER_SIZE + (s.numBuckets + 1) * 4;

            ESP_LOGI(TAG, "Opened WiFi geo DB %s: %u records, %u buckets",
                     path, (unsigned)s.count, (unsigned)s.numBuckets);
            return true;
        }

        static void _CloseUnlocked()
        {
            DbState &s = _DbState();
            if (s.file)
            {
                s.file.close();
            }
        }

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

}
