#pragma once

#include <ArduinoJson.h>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>

#include "LoraMessageInterface.hpp"
#include "LoraChannelPlan.h"
#include "EventHandler.h"
#include "SystemUtilities.hpp"
#include "SettingsInterface.hpp"

namespace LoraModule
{
    using MessageCreator = std::shared_ptr<LoraMessageInterface>(*)(JsonObject& payload);

    namespace
    {
        const size_t LORA_MESSAGE_QUEUE_LENGTH = 8;
    }

    // Façade between the UI/app and LoraModule::Manager (the MeshCore engine).
    // The UI only ever touches this class; Manager reaches back in for the
    // send queue, the message-type registry, the echo count, and the two
    // settings seams (ChannelKeyHandler, RequestChannel).
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

        // Queues a message for sending. Clones the message internally; the mesh
        // task (Manager::Loop) drains the queue and floods a group datagram.
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

        // Echo count = how many times our own last broadcast has come back to us.
        // Bumped from MeshTables::wasSeen() when a dedup hit matches our outbound.
        static uint32_t GetEchoCount() { return EchoCount(); }
        static void     IncrementEchoCount() { EchoCount()++; }

        static bool RegisterMessageType(uint32_t schemaGuid, MessageCreator creator)
        {
            auto& creators = Creators();
            if (creators.find(schemaGuid) != creators.end()) { return false; }
            creators[schemaGuid] = creator;
            return true;
        }

        // The wire format prefixes the msgpack payload with a 1-byte type tag
        // (the low byte of the schema GUID) instead of transmitting the FNV
        // hash. These map that tag back to a registered type on receive.
        // Collision-free with a handful of message types; revisit if that grows.
        static MessageCreator CreatorForTag(uint8_t tag)
        {
            for (auto& kv : Creators())
            {
                if ((kv.first & 0xFFu) == tag) { return kv.second; }
            }
            return nullptr;
        }

        static uint32_t GuidForTag(uint8_t tag)
        {
            for (auto& kv : Creators())
            {
                if ((kv.first & 0xFFu) == tag) { return kv.first; }
            }
            return 0;
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

        static constexpr const char* SETTING_LORA_PASSWORD = "Channel Key";
        static constexpr size_t      LORA_PASSWORD_MAX_LEN = 21;

        // Distinct from SETTING_LORA_PASSWORD above: that one is the group
        // "chatroom" secret, this one is the radio frequency. Both are short
        // enough to survive NVS's 15-character key limit.
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

            // No "Num Broadcasts" — MeshCore owns retry + airtime budgeting.
            // No "Repeat" — mobile nodes always relay (Manager::SetRepeat, hardcoded
            // true in BootstrapLora); a user who toggled it off and moved would
            // silently break the mesh.
        }

        // The settings pipeline hands "Channel Key" to Manager through this seam
        // (Manager registers itself at construction). A direct call would be a
        // circular include: Manager includes this header.
        using ChannelKeyApplier = void (*)(const std::string&);

        static ChannelKeyApplier& ChannelKeyHandler()
        {
            static ChannelKeyApplier fn = nullptr;
            return fn;
        }

        static void UpdateSettings(JsonDocument& settings)
        {
            if (!settings[SETTING_LORA_PASSWORD].isNull())
            {
                // "Channel Key" has exactly one consumer: derive the 32-byte group
                // secret + selector and hand them to Manager (which owns the
                // empty-key warning). PBKDF2 (~10k SHA-256 iters, ~70 ms) runs on
                // the settings task; the mesh task picks the new channel up within
                // one 20 ms tick.
                const std::string pw = settings[SETTING_LORA_PASSWORD].as<std::string>();
                if (ChannelKeyHandler() != nullptr) { ChannelKeyHandler()(pw); }
            }

            UserName() = settings["User Name"].as<std::string>();

            RequestChannel(settings[SETTING_LORA_CHANNEL] | LORA_CHANNEL_DEFAULT);
        }

        // ---------------------------------------------------------------------
        // Channel selection
        // ---------------------------------------------------------------------
        // Radio registers are only touched from Manager::Loop() (the mesh task).
        // Settings updates arrive on the display/RPC task, so a channel change is
        // published here and drained by the mesh task each wake. Publishing
        // RadioTaskHandle() lets a change apply on the next notify rather than
        // waiting out the 20 ms tick. No ActiveChannel short-circuit: that field
        // is only updated once the mesh task has actually retuned, so comparing
        // against it would swallow a repeat request.
        static void RequestChannel(int channel)
        {
            if (!IsValidChannel(channel))
            {
                ESP_LOGW(TAG, "Ignoring out-of-range channel %d", channel);
                return;
            }

            PendingChannel().store(channel);

            auto handle = RadioTaskHandle();
            if (handle != nullptr) { xTaskNotifyGive(handle); }
        }

        // Returns the channel to switch to, or 0 when nothing is pending.
        // Clears the request. Called only by the mesh task.
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

        // Published by Manager::Loop() so RequestChannel can wake it.
        static TaskHandle_t& RadioTaskHandle()
        {
            static TaskHandle_t h = nullptr;
            return h;
        }

    private:
        static std::atomic<int>& PendingChannel()
        {
            static std::atomic<int> ch{0};
            return ch;
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
