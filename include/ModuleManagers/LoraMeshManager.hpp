#pragma once

// MeshCore-backed replacement for the hand-rolled flood engine in
// LoraManager.hpp. Phase 1 of the migration (see meshcoremigrationplan.md):
// this is the skeleton -- identity, radio/dispatcher bring-up, the single
// wait-discipline task, the forwarding + backoff policy overrides, and a
// bench hook for a hardcoded group-datagram round-trip. Phase 2 wires
// PingMessage and the real "Channel Key"-derived GroupChannel through the
// LoraModule::Utilities facade.
//
// Kept as a separate class (LoraModule::MeshManager) and behind the
// USE_MESHCORE_LORA build flag while both stacks coexist; a later commit
// collapses it into LoraModule::Manager per the plan.

#include <Arduino.h>
#include <cstring>
#include <functional>
#include <LittleFS.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>          // StdRNG, ArduinoMillis
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/IdentityStore.h>

#include "HelperClasses/Mesh/MeshTables.hpp"
#include "HelperClasses/Mesh/MeshTimeClock.hpp"
#include "SystemUtilities.hpp"

namespace LoraModule
{
    namespace
    {
        constexpr int      MESH_PACKET_POOL      = 16;
        constexpr uint32_t MESH_LOOP_MAX_WAIT_MS = 20;   // plan 6.3 tick cap
        constexpr int      MESH_LOOPS_PER_WAKE   = 3;     // one pkt/dir per loop()

        // RSSI-inverse relay backoff, ported from LoraManager.hpp:132-152.
        constexpr int      RSSI_BACKOFF_MIN_DBM = -130;
        constexpr int      RSSI_BACKOFF_MAX_DBM = -80;
        constexpr uint32_t RSSI_BACKOFF_MAX_MS  = 2000;
        constexpr uint32_t RELAY_JITTER_MS      = 500;
    }

    class MeshManager : public mesh::Mesh
    {
    public:
        static constexpr const char* TAG = "LoraMesh";

        MeshManager(mesh::Radio& radio,
                    mesh::MillisecondClock& ms,
                    StdRNG& rng,
                    MeshTimeClock& rtc,
                    StaticPoolPacketManager& pkts,
                    MeshTables& tables)
            : mesh::Mesh(radio, ms, rng, rtc, pkts, tables),
              _tables(tables)
        {
        }

        // Loads or creates the Ed25519 identity, derives the legacy 32-bit
        // DeviceID from the pubkey prefix (keeps PingMessage::SenderTag() and
        // every uint32_t-sender map working), then brings up dispatcher + radio.
        bool Begin()
        {
            IdentityStore store(LittleFS, "/mesh");
            store.begin();

            if (!store.load("identity", self_id))
            {
                ESP_LOGW(TAG, "no stored identity - generating one");
                self_id = mesh::LocalIdentity(getRNG());
                if (!store.save("identity", self_id))
                {
                    ESP_LOGE(TAG, "failed to persist new identity");
                }
            }

            uint32_t devId;
            memcpy(&devId, self_id.pub_key, sizeof(devId));
            System_Utils::DeviceID = devId;
            ESP_LOGI(TAG, "identity ready, DeviceID 0x%08X", (unsigned)devId);

            _InstallTestChannel();

            mesh::Mesh::begin();   // Dispatcher::begin() -> _radio->begin()
            ESP_LOGI(TAG, "mesh up");
            return true;
        }

        // ------------------------------------------------------------------
        // Wait-discipline task body (plan 6.3). Replaces both RadioTask and
        // SendQueueTask. Dispatcher::loop() never blocks and its outbound
        // scheduling is time-driven, so we cannot sleep on portMAX_DELAY --
        // wake on either the ~20 ms cap or an app-initiated send notify.
        // ------------------------------------------------------------------
        void Loop()
        {
            MeshTaskHandle() = xTaskGetCurrentTaskHandle();

            for (;;)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MESH_LOOP_MAX_WAIT_MS));
                for (int i = 0; i < MESH_LOOPS_PER_WAKE; ++i)
                {
                    loop();   // mesh::Mesh::loop -> Dispatcher::loop
                }
                if (OnLoopTick) { OnLoopTick(); }
            }
        }

        // Optional per-wake hook, run on the mesh task after loop(). Phase 1
        // uses it for a bench beacon; keeps app-specific test traffic out of
        // this library class.
        std::function<void()> OnLoopTick;

        // Published so LoraUtils::SendMessage() (display/RPC task) can wake the
        // mesh task right after handing MeshCore a packet.
        static TaskHandle_t& MeshTaskHandle()
        {
            static TaskHandle_t h = nullptr;
            return h;
        }

        static void NotifyMeshTask()
        {
            TaskHandle_t h = MeshTaskHandle();
            if (h != nullptr) { xTaskNotifyGive(h); }
        }

        // ------------------------------------------------------------------
        // Phase 1 bench hook: push a raw group datagram onto the shared test
        // channel. Marks it as our own outbound first so MeshTables counts the
        // echoes. Phase 2 replaces this with PingMessage via the facade.
        // ------------------------------------------------------------------
        void SendTestDatagram(const uint8_t* data, size_t len)
        {
            mesh::Packet* pkt =
                createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, _testChannel, data, len);
            if (pkt == nullptr)
            {
                ESP_LOGE(TAG, "createGroupDatagram null (pool empty / too long)");
                return;
            }
            _tables.setOwnOutbound(pkt);
            sendFlood(pkt);
            NotifyMeshTask();
        }

        uint32_t EchoCount() const { return LoraModule::Utilities::GetEchoCount(); }

    protected:
        // Closed relay network: forward everything. MeshCore's default returns
        // false -> a silent one-hop mesh. Phase 2 gates this on a "Repeat"
        // BoolSetting.
        bool allowPacketForward(const mesh::Packet* /*packet*/) override
        {
            return _repeat;
        }

        // Weak signal => likely a distant, better-placed relay => shorter wait,
        // so it transmits first. Jitter because co-located nodes otherwise
        // clamp to identical delays and collide (LoraManager.hpp:132-152).
        uint32_t getRetransmitDelay(const mesh::Packet* /*packet*/) override
        {
            int rssi = static_cast<int>(_radio->getLastRSSI());
            int clamped = rssi < RSSI_BACKOFF_MIN_DBM ? RSSI_BACKOFF_MIN_DBM
                        : (rssi > RSSI_BACKOFF_MAX_DBM ? RSSI_BACKOFF_MAX_DBM : rssi);
            uint32_t base = static_cast<uint32_t>(clamped - RSSI_BACKOFF_MIN_DBM)
                          * RSSI_BACKOFF_MAX_MS
                          / (RSSI_BACKOFF_MAX_DBM - RSSI_BACKOFF_MIN_DBM);
            return base + getRNG()->nextInt(0, RELAY_JITTER_MS);
        }

        float getAirtimeBudgetFactor() const override { return 1.0f; }

        int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[],
                                 int max_matches) override
        {
            if (max_matches < 1) { return 0; }
            if (memcmp(hash, _testChannel.hash, PATH_HASH_SIZE) != 0) { return 0; }
            channels[0] = _testChannel;
            return 1;
        }

        void onGroupDataRecv(mesh::Packet* /*packet*/, uint8_t type,
                             const mesh::GroupChannel& /*channel*/,
                             uint8_t* data, size_t len) override
        {
            ESP_LOGI(TAG, "GRP_DATA type=%u len=%u: %.*s",
                     type, (unsigned)len, (int)len, reinterpret_cast<const char*>(data));
        }

    private:
        // Phase 1 stand-in for the Channel Key-derived secret. Any two boards
        // running this build share it. Plan 5.1: the 1-byte hash is only a
        // selector; encrypt-then-MAC over the 32-byte secret is the real gate.
        void _InstallTestChannel()
        {
            static const uint8_t kSecret[PUB_KEY_SIZE] = {
                0x9e,0x2f,0x71,0xc4,0x0a,0x53,0x18,0xd6,
                0x84,0x3b,0xf0,0x27,0xcd,0x11,0x6a,0x99,
                0x5c,0xe8,0x40,0x77,0x22,0xbb,0x0d,0xa1,
                0x38,0x64,0x9f,0x1e,0x7d,0xc9,0x05,0x53
            };
            memcpy(_testChannel.secret, kSecret, PUB_KEY_SIZE);

            uint8_t digest[32];
            mesh::Utils::sha256(digest, sizeof(digest), kSecret, PUB_KEY_SIZE);
            memcpy(_testChannel.hash, digest, PATH_HASH_SIZE);
        }

        MeshTables&        _tables;
        mesh::GroupChannel _testChannel {};
        bool               _repeat = true;
    };
}
