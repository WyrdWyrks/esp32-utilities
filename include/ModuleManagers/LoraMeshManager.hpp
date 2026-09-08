#pragma once

// MeshCore-backed replacement for the hand-rolled flood engine in
// LoraManager.hpp (migration plan, Phases 1-2). Behind USE_MESHCORE_LORA while
// both stacks coexist; a later commit collapses it into LoraModule::Manager.
//
// Phase 1: identity, radio/dispatcher bring-up, the single wait-discipline
//          task, forwarding + RSSI backoff policy.
// Phase 2: real "Channel Key"-derived GroupChannel; app PingMessages ride the
//          channel through the unchanged LoraModule::Utilities facade --
//          SendMessage() still enqueues, the mesh task drains that queue,
//          serialises [1-byte type tag][msgpack], and floods a group datagram;
//          onGroupDataRecv() reverses it and fires MessageTypeReceived().

#include <Arduino.h>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <LittleFS.h>

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>          // StdRNG, ArduinoMillis
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/IdentityStore.h>

#include "HelperClasses/Mesh/MeshTables.hpp"
#include "HelperClasses/Mesh/MeshTimeClock.hpp"
#include "LoraMessageInterface.hpp"
#include "EncryptionUtils.hpp"
#include "LoraUtilities.hpp"
#include "FilesystemUtils.h"
#include "SystemUtilities.hpp"

namespace LoraModule
{
    namespace
    {
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
            _instance = this;

            // Register as the one consumer of "Channel Key" (see ApplyChannelKey).
            LoraModule::Utilities::ChannelKeyHandler() = &ApplyChannelKey;

            // Apply a channel derived before Begin() (see ApplyChannelKey). The
            // mesh task does not exist yet, so publishing here races nothing.
            if (_PendingValid())
            {
                memcpy(_channel.secret, _PendingSecret(), PUB_KEY_SIZE);
                memcpy(_channel.hash, _PendingHash(), PATH_HASH_SIZE);
            }
        }

        // Loads or creates the Ed25519 identity, derives the legacy 32-bit
        // DeviceID from the pubkey prefix (keeps PingMessage::SenderTag() and
        // every uint32_t-sender map working), installs the group channel from
        // the persisted Channel Key, then brings up dispatcher + radio.
        bool Begin()
        {
            // Create the facade's send queue BEFORE anything can call
            // Utilities::SendMessage(). In this build that is MeshManager::Loop()
            // draining it — but Loop() only runs once the app registers the mesh
            // task, which happens after Begin(). Doing Init() inside Loop() would
            // mean every send before the task starts fails with "queue not ready"
            // (the UI shows "Failed to send").
            LoraModule::Utilities::Init();

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

            SetChannelKey(FilesystemModule::Utilities::FetchStringSetting(
                LoraModule::Utilities::SETTING_LORA_PASSWORD, ""));

            mesh::Mesh::begin();   // Dispatcher::begin() -> _radio->begin()
            ESP_LOGI(TAG, "mesh up");
            return true;
        }

        // Rebuilds the group channel from a passphrase. PBKDF2 (shared salt) so
        // the same key yields the same 32-byte secret fleet-wide; the 1-byte
        // channel.hash is just a cleartext selector -- encrypt-then-MAC over the
        // secret is the real separation (plan 5.1). Safe to call at runtime when
        // the setting changes: derive into locals first and publish with two
        // aligned stores, so the mesh task (which reads _channel locklessly) can
        // never observe a mismatched secret/selector pair — the worst case is one
        // dropped packet while the 32-byte copy lands mid-flight.
        void SetChannelKey(const std::string& passphrase)
        {
            uint8_t secret[PUB_KEY_SIZE];
            uint8_t hash[PATH_HASH_SIZE];
            EncryptionUtils::DeriveKey(passphrase, secret, PUB_KEY_SIZE);

            uint8_t digest[32];
            mesh::Utils::sha256(digest, sizeof(digest), secret, PUB_KEY_SIZE);
            memcpy(hash, digest, PATH_HASH_SIZE);

            if (passphrase.empty())
            {
                // Empty key = every default-config device shares one well-known
                // group secret. Traffic is still encrypted+MAC'd, but there is no
                // privacy against anyone who never set a key. Loud on purpose:
                // an empty field must not read as "encrypted".
                ESP_LOGW(TAG, "Channel Key EMPTY - running on the shared default "
                              "channel with a well-known secret. Set a Channel Key "
                              "for any real privacy.");
            }
            else
            {
                ESP_LOGI(TAG, "channel key set (custom), selector 0x%02X", hash[0]);
            }

            memcpy(_channel.secret, secret, PUB_KEY_SIZE);   // publish secret...
            memcpy(_channel.hash, hash, PATH_HASH_SIZE);     // ...then its selector
        }

        // Single consumer of the "Channel Key" setting in this build (registered
        // with LoraModule::Utilities::ChannelKeyHandler() below; invoked from
        // UpdateSettings on the settings task). Uses the live instance when Begin()
        // has run; before that, stashes the derived bytes — Begin()'s own
        // SetChannelKey() re-reads the persisted setting, so the stash is
        // belt-and-braces for ordering, not correctness.
        static void ApplyChannelKey(const std::string& passphrase)
        {
            if (_instance != nullptr)
            {
                _instance->SetChannelKey(passphrase);
                return;
            }

            EncryptionUtils::DeriveKey(passphrase, _PendingSecret(), PUB_KEY_SIZE);
            uint8_t digest[32];
            mesh::Utils::sha256(digest, sizeof(digest), _PendingSecret(), PUB_KEY_SIZE);
            memcpy(_PendingHash(), digest, PATH_HASH_SIZE);
            _PendingValid() = true;
        }

        // ------------------------------------------------------------------
        // Wait-discipline task body (plan 6.3). Replaces both RadioTask and
        // SendQueueTask. Dispatcher::loop() never blocks and its outbound
        // scheduling is time-driven, so we cannot sleep on portMAX_DELAY.
        // Also drains the facade's send queue here so createGroupDatagram/
        // sendFlood only ever run on this task, never racing loop().
        // ------------------------------------------------------------------
        void Loop()
        {
            MeshTaskHandle() = xTaskGetCurrentTaskHandle();
            _sendQueue = System_Utils::getQueue(LoraModule::Utilities::MessageSendQueueID());

            for (;;)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MESH_LOOP_MAX_WAIT_MS));

                _DrainSendQueue();

                for (int i = 0; i < MESH_LOOPS_PER_WAKE; ++i)
                {
                    loop();   // mesh::Mesh::loop -> Dispatcher::loop
                }
                if (OnLoopTick) { OnLoopTick(); }
            }
        }

        // Optional per-wake hook, run on the mesh task after loop().
        std::function<void()> OnLoopTick;

        // Published so LoraUtils::SendMessage() (display/RPC task) can wake the
        // mesh task right after enqueueing, instead of waiting out the tick.
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

        uint32_t EchoCount() const { return LoraModule::Utilities::GetEchoCount(); }

    protected:
        // Closed relay network: forward everything. MeshCore's default returns
        // false -> a silent one-hop mesh. (Phase 2 plan also wants a "Repeat"
        // BoolSetting gate; not wired yet.)
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

        // Single channel. A non-match returns 0, which only skips the recv
        // callback -- the packet is still relayed by the route layer, preserving
        // the "relay other chatrooms' traffic" behaviour (plan Phase 2 note).
        int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[],
                                 int max_matches) override
        {
            if (max_matches < 1) { return 0; }
            if (memcmp(hash, _channel.hash, PATH_HASH_SIZE) != 0) { return 0; }
            channels[0] = _channel;
            return 1;
        }

        // [0]      = type tag (low byte of the schema GUID)
        // [1..len) = msgpack: base routing fields + "p" payload map
        void onGroupDataRecv(mesh::Packet* /*packet*/, uint8_t /*type*/,
                             const mesh::GroupChannel& /*channel*/,
                             uint8_t* data, size_t len) override
        {
            if (len < 2) { return; }
            const uint8_t tag = data[0];

            JsonDocument doc;
            if (deserializeMsgPack(doc, data + 1, len - 1) != DeserializationError::Ok)
            {
                ESP_LOGW(TAG, "recv: msgpack decode failed (tag 0x%02X, %u bytes)",
                         tag, (unsigned)len);
                return;
            }

            MessageCreator creator = LoraModule::Utilities::CreatorForTag(tag);
            uint32_t guid = LoraModule::Utilities::GuidForTag(tag);
            if (creator == nullptr || guid == 0)
            {
                ESP_LOGW(TAG, "recv: no message type for tag 0x%02X", tag);
                return;
            }

            JsonObject payload = doc[LoraMessageInterface::KEY_PAYLOAD].as<JsonObject>();
            if (payload.isNull()) { return; }

            auto msg = creator(payload);          // deserialises the payload map
            if (!msg) { return; }
            msg->deserialize(doc);                // base fields: sender/msgID/time/date
            if (!msg->IsValid()) { return; }

            // MeshCore's dedup dropped repeats before we got here, so every
            // delivery is new.
            LoraModule::Utilities::MessageTypeReceived(guid).Invoke(msg, true);
        }

    private:
        void _DrainSendQueue()
        {
            if (_sendQueue == nullptr) { return; }

            std::shared_ptr<LoraMessageInterface>* wrapper = nullptr;
            while (xQueueReceive(_sendQueue, &wrapper, 0) == pdTRUE)
            {
                if (wrapper != nullptr)
                {
                    _SendLoraMessage(*wrapper);
                    delete wrapper;
                }
            }
        }

        // mesh task only.
        void _SendLoraMessage(const std::shared_ptr<LoraMessageInterface>& msg)
        {
            if (!msg) { return; }

            JsonDocument doc;
            if (!msg->serialize(doc))      // base fields + "p" payload; iv is zero
            {
                ESP_LOGE(TAG, "send: serialize failed for msgID 0x%08X", (unsigned)msg->msgID);
                return;
            }

            uint8_t buf[MAX_GROUP_DATA_LENGTH];
            buf[0] = static_cast<uint8_t>(msg->SchemaGuid() & 0xFFu);
            size_t n = serializeMsgPack(doc, buf + 1, sizeof(buf) - 1);
            if (n == 0)
            {
                ESP_LOGE(TAG, "send: msgpack overflow (>%u bytes)", (unsigned)(sizeof(buf) - 1));
                return;
            }

            mesh::Packet* pkt =
                createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, _channel, buf, n + 1);
            if (pkt == nullptr)
            {
                ESP_LOGE(TAG, "send: createGroupDatagram null (pool empty?)");
                return;
            }

            _tables.setOwnOutbound(pkt);   // so MeshTables counts the echoes
            sendFlood(pkt);
            ESP_LOGI(TAG, "sent msgID 0x%08X  tag 0x%02X  %u bytes",
                     (unsigned)msg->msgID, buf[0], (unsigned)(n + 1));
        }

        MeshTables&        _tables;
        mesh::GroupChannel _channel {};
        bool               _repeat = true;
        QueueHandle_t      _sendQueue = nullptr;

        // Sole MeshManager instance (one per firmware, wired by BootstrapLora).
        // Lets LoraUtilities::UpdateSettings() reach SetChannelKey() without the
        // library's generic façade hard-coding a dependency on this class.
        static inline MeshManager* _instance = nullptr;

        // Channel bytes derived before any instance existed. Meyers singletons so
        // there is no .cpp — the pattern this codebase uses for all new statics.
        static uint8_t* _PendingSecret() { static uint8_t s[PUB_KEY_SIZE]{}; return s; }
        static uint8_t* _PendingHash()   { static uint8_t h[PATH_HASH_SIZE]{}; return h; }
        static bool&    _PendingValid()  { static bool v = false; return v; }
    };
}
