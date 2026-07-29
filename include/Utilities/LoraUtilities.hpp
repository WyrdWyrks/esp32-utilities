#pragma once

#include <ArduinoJson.h>
#include <atomic>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>

#include "LoraMessageInterface.hpp"
#include "LoraChannelPlan.h"
#include "EncryptionUtils.hpp"
#include "EventHandler.h"
#include "SystemUtilities.hpp"
#include "SettingsInterface.hpp"

namespace LoraModule
{
    using MessageCreator = std::shared_ptr<LoraMessageInterface>(*)(JsonObject& payload);

    namespace
    {
        const size_t LORA_MESSAGE_QUEUE_LENGTH  = 8;
        const uint8_t LORA_DEFAULT_SEND_ATTEMPTS = 3;
    }

    class Utilities
    {
    public:
        static constexpr const char* TAG = "LoraUtils";

        static void Init()
        {
            if (MessageSendQueueID() != -1) { return; }

            static StaticQueue_t queueBuffer;
            static uint8_t queueStorage[LORA_MESSAGE_QUEUE_LENGTH * sizeof(std::shared_ptr<LoraMessageInterface>*)];
            MessageSendQueueID() = System_Utils::registerQueue(
                LORA_MESSAGE_QUEUE_LENGTH,
                sizeof(std::shared_ptr<LoraMessageInterface>*),
                queueStorage,
                queueBuffer);
        }

        // Queues a message for sending. Clones the message internally.
        static bool SendMessage(std::shared_ptr<LoraMessageInterface> msg)
        {
            if (!msg) { return false; }
            if (MessageSendQueueID() == -1) { return false; }

            if (msg->sender == System_Utils::DeviceID)
            {
                SetMyLastBroadcast(msg);
            }

            auto* wrapper = new std::shared_ptr<LoraMessageInterface>(msg->clone());
            return System_Utils::sendToQueue(MessageSendQueueID(), &wrapper, 1000);
        }

        // Returns a shared_ptr to the last broadcast (shared ownership, no clone needed).
        static std::shared_ptr<LoraMessageInterface> MyLastBroadcast()
        {
            if (xSemaphoreTake(MessageAccessMutex(), portMAX_DELAY) == pdTRUE)
            {
                auto msg = MyLastBroadcastMsg();
                xSemaphoreGive(MessageAccessMutex());
                return msg;
            }
            return nullptr;
        }

        static void SetMyLastBroadcast(std::shared_ptr<LoraMessageInterface> msg)
        {
            if (xSemaphoreTake(MessageAccessMutex(), portMAX_DELAY) == pdTRUE)
            {
                MyLastBroadcastMsg() = msg;
                xSemaphoreGive(MessageAccessMutex());
            }
            EchoCount() = 0;
            MyLastBroadcastChanged().Invoke();
        }

        static uint32_t GetEchoCount() { return EchoCount(); }
        static void     IncrementEchoCount() { EchoCount()++; }

        // Records a senderId→msgId pair in the routing map for deduplication.
        // Call after MessageExists() to mark this message as seen.
        static void RecordRouting(uint32_t senderId, uint32_t msgId)
        {
            if (xSemaphoreTake(MessageAccessMutex(), portMAX_DELAY) == pdTRUE)
            {
                RoutingMap()[senderId] = msgId;
                xSemaphoreGive(MessageAccessMutex());
            }
        }

        // Returns true if we have already recorded a message with this exact
        // (senderId, msgId) pair — used to compute isNew before RecordRouting.
        static bool MessageExists(uint32_t senderId, uint32_t msgId)
        {
            if (xSemaphoreTake(MessageAccessMutex(), portMAX_DELAY) == pdTRUE)
            {
                auto& m = RoutingMap();
                auto it = m.find(senderId);
                bool exists = (it != m.end() && it->second == msgId);
                xSemaphoreGive(MessageAccessMutex());
                return exists;
            }
            return false;
        }

        static bool RegisterMessageType(uint32_t schemaGuid, MessageCreator creator)
        {
            auto& creators = Creators();
            if (creators.find(schemaGuid) != creators.end()) { return false; }
            creators[schemaGuid] = creator;
            return true;
        }

        static std::shared_ptr<LoraMessageInterface> DeserializeMessage(const uint8_t* buffer, size_t len)
        {
            JsonDocument doc;
            JsonDocument payloadDoc;
            JsonObject payload;

            // Single msgpack document. The routing header is always plaintext; the "p" field is
            // either a map (plaintext payload) or a bin of raw AES-CBC ciphertext (encrypted).
            // The IV lives in the header as the "v" bin.
            if (deserializeMsgPack(doc, buffer, len) != DeserializationError::Ok)
            {
                return nullptr;
            }

            JsonVariant payloadVar = doc[LoraMessageInterface::KEY_PAYLOAD];

            if (payloadVar.is<JsonObject>())
            {
                // Plaintext payload. If we have a key set this message belongs to a different
                // "chatroom" (or is unencrypted noise) — never dispatch plaintext when encrypting.
                // LoraManager still routes it via ReadBaseFields + RelayMessage.
                if (EncryptionEnabled()) { return nullptr; }
                payload = payloadVar.as<JsonObject>();
            }
            else
            {
                // Encrypted payload. We can only read it with a matching key; a different
                // chatroom's key fails the PKCS7 check in Decrypt and returns nullptr.
                if (!EncryptionEnabled()) { return nullptr; }

                MsgPackBinary cipher = payloadVar.as<MsgPackBinary>();
                if (!cipher.data()) { return nullptr; }

                uint8_t iv[EncryptionUtils::IV_SIZE]{};
                auto ivBin = doc[LoraMessageInterface::KEY_IV].as<MsgPackBinary>();
                if (!ivBin.data() || ivBin.size() != EncryptionUtils::IV_SIZE) { return nullptr; }
                memcpy(iv, ivBin.data(), EncryptionUtils::IV_SIZE);

                uint8_t plaintext[MSG_BASE_SIZE];
                size_t plaintextLen = 0;
                if (!EncryptionUtils::Decrypt(reinterpret_cast<const uint8_t*>(cipher.data()),
                                              cipher.size(), plaintext, plaintextLen,
                                              EncryptionKey(), iv))
                {
                    return nullptr;
                }

                if (deserializeMsgPack(payloadDoc, plaintext, plaintextLen) != DeserializationError::Ok)
                {
                    return nullptr;
                }
                payload = payloadDoc.as<JsonObject>();
            }

            if (payload.isNull()) { return nullptr; }

            uint32_t guid = computeSchemaGuid(payload);
            auto& creators = Creators();
            auto it = creators.find(guid);
            if (it == creators.end()) { return nullptr; }

            auto msg = it->second(payload);
            if (!msg) { return nullptr; }

            msg->deserialize(doc);
            if (!msg->IsValid()) { return nullptr; }

            return msg;
        }

        static bool SerializeMessage(const std::shared_ptr<LoraMessageInterface>& msg,
                                     uint8_t* buffer, size_t& outLen)
        {
            if (EncryptionEnabled())
            {
                ESP_LOGI("LoraUtilities", "Serializing encrypted message");
                // Generate a fresh random IV for this message and store it on the object
                // so it is serialized into the base header along with the other fields.
                EncryptionUtils::GenerateIV(msg->iv);
            }
            else
            {
                memset(msg->iv, 0, EncryptionUtils::IV_SIZE);
            }

            JsonDocument doc;
            if (!msg->serialize(doc)) 
            { 
                ESP_LOGE("LoraUtilities", "Failed to serialize message");
                return false; 
            }
            else
            {
                std::string debugStr;
                serializeJson(doc, debugStr);
            }

            if (EncryptionEnabled())
            {
                // Serialize the payload object on its own and encrypt it, then swap the payload
                // map in the document for the raw ciphertext as a msgpack bin. The result is a
                // single self-describing document — no 0xEE framing — where "p" being a bin
                // (rather than a map) signals encryption.
                JsonObject payloadObj = doc[LoraMessageInterface::KEY_PAYLOAD].as<JsonObject>();

                uint8_t plaintext[MSG_BASE_SIZE];
                size_t plaintextLen = serializeMsgPack(payloadObj, plaintext, sizeof(plaintext));

                uint8_t ciphertext[MSG_BASE_SIZE + 16];
                size_t ciphertextLen = 0;
                if (!EncryptionUtils::Encrypt(plaintext, plaintextLen, ciphertext, ciphertextLen,
                                              EncryptionKey(), msg->iv))
                {
                    return false;
                }

                doc.remove(LoraMessageInterface::KEY_PAYLOAD);
                doc[LoraMessageInterface::KEY_PAYLOAD] = MsgPackBinary(ciphertext, ciphertextLen);
            }

            outLen = serializeMsgPack(doc, buffer, MSG_BASE_SIZE);
            return outLen > 0;
        }

        static bool MessagePackSanityCheck(JsonDocument& doc)
        {
            uint8_t buffer[MSG_BASE_SIZE];
            size_t len = serializeMsgPack(doc, buffer, sizeof(buffer));
            JsonDocument doc2;
            auto rc = deserializeMsgPack(doc2, buffer, len);
            if (rc != DeserializationError::Ok)
            {
                ESP_LOGE(TAG, "MessagePackSanityCheck failed: %s", rc.c_str());
                return false;
            }
            return true;
        }

        // Per-type event map — keyed by schema GUID.
        // Application registers handlers via MessageTypeReceived(GUID) += ...
        static std::unordered_map<uint32_t, EventHandler<std::shared_ptr<LoraMessageInterface>, bool>>& MessageEvents()
        {
            static std::unordered_map<uint32_t, EventHandler<std::shared_ptr<LoraMessageInterface>, bool>> events;
            return events;
        }

        static EventHandler<std::shared_ptr<LoraMessageInterface>, bool>& MessageTypeReceived(uint32_t schemaGuid)
        {
            return MessageEvents()[schemaGuid];
        }

        // Fired when SetMyLastBroadcast is called (i.e. the user sends a new message).
        // Subscribe to reset application-layer state (e.g. echo counts).
        static EventHandler<>& MyLastBroadcastChanged()
        {
            static EventHandler<> e;
            return e;
        }

        static bool MyLastBroadcastExists() { return MyLastBroadcastMsg() != nullptr; }

        // Getters / setters via Meyers singletons
        static int& MessageSendQueueID()
        {
            static int id = -1;
            return id;
        }

        static std::string& UserName()
        {
            static std::string name = "User";
            return name;
        }

        static uint8_t& NodeID()
        {
            static uint8_t id = 0;
            return id;
        }

        static uint8_t& DefaultSendAttempts()
        {
            static uint8_t n = LORA_DEFAULT_SEND_ATTEMPTS;
            return n;
        }

        static constexpr const char* SETTING_LORA_PASSWORD = "Channel Key";
        static constexpr size_t      LORA_PASSWORD_MAX_LEN = 21;

        // Distinct from SETTING_LORA_PASSWORD above: that one is the encryption
        // "chatroom", this one is the radio frequency. Both are short enough to
        // survive NVS's 15-character key limit.
        static constexpr const char* SETTING_LORA_CHANNEL = "LoRa Channel";

        static void GenerateDefaultSettings(std::vector<std::shared_ptr<FilesystemModule::SettingsInterface>>& settings)
        {
            auto pw = std::make_shared<FilesystemModule::StringSetting>(
                SETTING_LORA_PASSWORD, "", LORA_PASSWORD_MAX_LEN);
            settings.push_back(pw);

            // Replaces the old "Frequency" float, whose 0.2 MHz steps were
            // meaningless at 500 kHz bandwidth. See LoraChannelPlan.h.
            std::vector<std::string> channelLabels;
            std::vector<int>         channelValues;
            for (int ch = 1; ch <= LORA_CHANNEL_COUNT; ++ch)
            {
                channelLabels.push_back(ChannelLabel(ch));
                channelValues.push_back(ch);
            }
            auto channel = std::make_shared<FilesystemModule::EnumSetting>(
                SETTING_LORA_CHANNEL, LORA_CHANNEL_DEFAULT,
                std::move(channelLabels), std::move(channelValues));
            settings.push_back(channel);

            auto broadcastAttempts = std::make_shared<FilesystemModule::IntSetting>("Num Broadcasts", 3, 1, 5, 1);
            settings.push_back(broadcastAttempts);
        }

        static void UpdateSettings(JsonDocument& settings)
        {
            if (!settings[SETTING_LORA_PASSWORD].isNull())
            {
                const std::string pw = settings[SETTING_LORA_PASSWORD].as<std::string>();
                EncryptionEnabled() = !pw.empty();
                if (EncryptionEnabled())
                {
                    // Empty password → plaintext mode; DeriveKey must never be called with "".
                    EncryptionUtils::DeriveKey(pw, EncryptionKey());
                }
            }

            UserName() = settings["User Name"].as<std::string>();
            DefaultSendAttempts() = settings["Num Broadcasts"] | (uint8_t)3;

            RequestChannel(settings[SETTING_LORA_CHANNEL] | LORA_CHANNEL_DEFAULT);
        }

        // ---------------------------------------------------------------------
        // Channel selection
        // ---------------------------------------------------------------------
        // Radio registers are only ever touched from the Manager's radio task,
        // but settings updates arrive on the display or RPC task. So a channel
        // change is published here and applied by the radio task — this records
        // the request and pokes the task awake.
        //
        // At boot the settings pass runs before the radio task exists (the
        // notify below no-ops against a null handle), which is why RadioTask
        // also drains the request once on entry.

        static void RequestChannel(int channel)
        {
            if (!IsValidChannel(channel))
            {
                ESP_LOGW(TAG, "Ignoring out-of-range channel %d", channel);
                return;
            }

            if (channel == ActiveChannel()) { return; }

            PendingChannel().store(channel);

            auto handle = RadioTaskHandle();
            if (handle != nullptr) { xTaskNotifyGive(handle); }
        }

        // Returns the channel to switch to, or 0 when nothing is pending.
        // Clears the request. Called only by the radio task.
        static int TakePendingChannel()
        {
            return PendingChannel().exchange(0);
        }

        // The channel the radio is actually tuned to.
        static int& ActiveChannel()
        {
            static int ch = LORA_CHANNEL_DEFAULT;
            return ch;
        }

        // Published by Manager::RadioTask so RequestChannel can wake it.
        static TaskHandle_t& RadioTaskHandle()
        {
            static TaskHandle_t h = nullptr;
            return h;
        }

        // Reads sender / msgID / bouncesLeft from ANY message format (encrypted or plaintext)
        // without attempting decryption. Used by LoraManager to make routing decisions for
        // messages that cannot be dispatched (wrong chatroom).
        static bool ReadBaseFields(const uint8_t* buffer, size_t len,
                                   uint32_t& sender, uint32_t& msgID, uint8_t& bouncesLeft)
        {
            // The routing header is always plaintext top-level msgpack, regardless of whether
            // the "p" payload is an encrypted str — so a single deserialize covers both formats.
            JsonDocument doc;
            if (deserializeMsgPack(doc, buffer, len) != DeserializationError::Ok) 
            { 
                ESP_LOGE("LoraUtilities", "Failed to deserialize message");
                return false; 
            }
            else
            {
                std::string debugStr;
                serializeJson(doc, debugStr);
            }
            sender      = doc[LoraMessageInterface::KEY_FROM]         | 0u;
            msgID       = doc[LoraMessageInterface::KEY_MSG_ID]       | 0u;
            bouncesLeft = doc[LoraMessageInterface::KEY_BOUNCES_LEFT] | uint8_t(0);
            return sender != 0 && msgID != 0;
        }

        // Re-packs a received message with a decremented bouncesLeft for raw relay.
        // For encrypted messages the ciphertext is copied verbatim so downstream devices
        // with the correct key can still decrypt. Used when DeserializeMessage returns nullptr
        // (wrong chatroom) but the message should still be forwarded.
        static bool RelayMessage(const uint8_t* buffer, size_t len,
                                 uint8_t* outBuffer, size_t& outLen, uint8_t newBouncesLeft)
        {
            // One document round-trips both formats: the encrypted "p" and "v" bins (and the
            // plaintext "p" map) re-serialize byte-identical, so the ciphertext is preserved
            // for downstream nodes without any special-casing — only bouncesLeft changes.
            JsonDocument doc;
            if (deserializeMsgPack(doc, buffer, len) != DeserializationError::Ok) 
            { 
                ESP_LOGW("LoraUtilities", "Failed to deserialize message for relay");
                return false; 
            }
            else
            {
                std::string debugStr;
                serializeMsgPack(doc, debugStr);
            }
            doc[LoraMessageInterface::KEY_BOUNCES_LEFT] = newBouncesLeft;
            outLen = serializeMsgPack(doc, outBuffer, MSG_BASE_SIZE);
            return outLen > 0;
        }

    private:
        static std::atomic<int>& PendingChannel()
        {
            static std::atomic<int> ch{0};
            return ch;
        }

        static bool& EncryptionEnabled()
        {
            static bool e = false;
            return e;
        }

        static uint8_t* EncryptionKey()
        {
            static uint8_t k[EncryptionUtils::KEY_SIZE]{};
            return k;
        }

        // senderId → most-recently-seen msgId, for MessageExists() deduplication.
        static std::map<uint32_t, uint32_t>& RoutingMap()
        {
            static std::map<uint32_t, uint32_t> m;
            return m;
        }

        static std::shared_ptr<LoraMessageInterface>& MyLastBroadcastMsg()
        {
            static std::shared_ptr<LoraMessageInterface> msg;
            return msg;
        }

        static uint32_t& EchoCount()
        {
            static uint32_t n = 0;
            return n;
        }

        static std::unordered_map<uint32_t, MessageCreator>& Creators()
        {
            static std::unordered_map<uint32_t, MessageCreator> c;
            return c;
        }

        static SemaphoreHandle_t& MessageAccessMutex()
        {
            static StaticSemaphore_t buf;
            static SemaphoreHandle_t h = xSemaphoreCreateMutexStatic(&buf);
            return h;
        }
    };
}
