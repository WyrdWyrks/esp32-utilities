#pragma once

#include "LoraUtilities.hpp"
#include "FilesystemUtils.h"
#include "LoraDriverInterface.h"
#include <atomic>

namespace
{
    const size_t  MAX_MESSAGE_SIZE         = 512;
    const size_t  AFTER_SEND_BLOCK_TIME_MS = 50;
    const size_t  NUM_REBROADCAST_ATTEMPTS = 1;
    const size_t  MIN_SEND_DELAY_MS        = 100;
    const size_t  MAX_SEND_DELAY_MS        = 3000;
    const uint8_t MAX_BOUNCES_LEFT         = 5;
}

namespace LoraModule
{

class Manager
{
public:
    static constexpr const char* TAG = "LoraManager";

    Manager(LoraDriverInterface* driver) : _Driver(driver) {}

    bool Init()
    {
        if (_Driver == nullptr) { return false; }
        if (!_Driver->Init())  { return false; }

        LoraModule::Utilities::Init();

        _sendQueue = System_Utils::getQueue(LoraModule::Utilities::MessageSendQueueID());
        if (_sendQueue == nullptr) { return false; }

        return true;
    }

    void RadioTask()
    {
        // Self-register task handle so the DIO0 ISR and SendQueueTask can notify us
        _ReceiveTaskHandle = xTaskGetCurrentTaskHandle();

        // Publish it so a channel change from the settings task can wake us
        LoraModule::Utilities::RadioTaskHandle() = _ReceiveTaskHandle;

        // Settings are processed before this task exists, so the channel chosen
        // at boot is already sitting in the request slot — apply it before the
        // first RX rather than waiting for the next settings change.
        _ApplyPendingChannel();

        // Enter continuous receive mode — safe here because handle is now set
        _Driver->StartReceiving();

        while (true)
        {
            // Block until DIO0 ISR (packet received) or SendQueueTask (message to send) wakes us
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            // Try to read a received packet (data already buffered by library ISR)
            uint8_t buffer[MAX_MESSAGE_SIZE];
            size_t  len = 0;

            if (_Driver->ReceiveMessage(buffer, len, 0))
            {
                ESP_LOGI(TAG, "Received LoRa message: %d bytes", len);
                // Phase 1: Extract base fields for routing — works for any message format,
                // regardless of whether we can decrypt the payload.
                uint32_t routeSender = 0, routeMsgID = 0;
                uint8_t  routeBouncesLeft = 0;

                if (LoraModule::Utilities::ReadBaseFields(buffer, len,
                                                          routeSender, routeMsgID, routeBouncesLeft))
                {
                    if (routeBouncesLeft > MAX_BOUNCES_LEFT)
                    {
                        ESP_LOGW(TAG, "Clamping bouncesLeft %d -> %d from sender 0x%08X",
                                 routeBouncesLeft, MAX_BOUNCES_LEFT, routeSender);
                        routeBouncesLeft = MAX_BOUNCES_LEFT;
                    }

                    if (routeSender == System_Utils::DeviceID)
                    {
                        ESP_LOGI(TAG, "Message echoed back from this node — dropping");
                        LoraModule::Utilities::IncrementEchoCount();
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Received from sender 0x%08X  msgID 0x%08X  bouncesLeft %d",
                                 routeSender, routeMsgID, routeBouncesLeft);

                        bool shouldFwd = ShouldMessageBeForwarded(routeSender, routeMsgID, routeBouncesLeft);

                        if (!shouldFwd)
                        {
                            auto it = _lastReceivedMessages.find(routeSender);
                            if (routeBouncesLeft == 0)
                            {
                                ESP_LOGI(TAG, "Not forwarding — bouncesLeft == 0");
                            }
                            else if (it != _lastReceivedMessages.end() && it->second == routeMsgID)
                            {
                                ESP_LOGI(TAG, "Not forwarding — duplicate (last seen msgID 0x%08X)", it->second);
                            }
                        }

                        bool isNew = !LoraModule::Utilities::MessageExists(routeSender, routeMsgID);
                        LoraModule::Utilities::RecordRouting(routeSender, routeMsgID);

                        // Phase 2: Attempt full deserialization + application dispatch.
                        // Returns nullptr if encryption keys don't match ("different chatroom").
                        // Routing above has already happened regardless.
                        auto msg = LoraModule::Utilities::DeserializeMessage(buffer, len);
                        if (msg != nullptr)
                        {
                            auto& events = LoraModule::Utilities::MessageEvents();
                            auto evIt = events.find(msg->SchemaGuid());
                            if (evIt != events.end())
                            {
                                evIt->second.Invoke(msg, isNew);
                            }
                        }

                        if (shouldFwd)
                        {
                            // Re-enter RX immediately so the radio stays live during the wait
                            _Driver->StartReceiving();

                            // RSSI-based backoff: weak signal = better relay candidate = shorter wait.
                            // Maps [-130, -80] dBm → [0, 2000] ms  (stronger signal = longer delay,
                            // letting distant nodes — which are better positioned — relay first).
                            int rssi = _Driver->PacketRssi();
                            int clampedRssi = rssi < -130 ? -130 : (rssi > -80 ? -80 : rssi);
                            uint32_t rssiDelayMs = static_cast<uint32_t>((clampedRssi + 130) * 2000 / 50);

                            ESP_LOGI(TAG, "Relay wait %u ms (RSSI %d dBm) — bouncesLeft %d -> %d",
                                     rssiDelayMs, rssi, routeBouncesLeft, routeBouncesLeft - 1);

                            if (rssiDelayMs > 0)
                            {
                                vTaskDelay(pdMS_TO_TICKS(rssiDelayMs));
                            }

                            if (msg != nullptr)
                            {
                                // Full message available — re-serialize cleanly via the send queue.
                                msg->bouncesLeft--;
                                LoraModule::Utilities::SendMessage(msg);
                            }
                            else
                            {
                                // Different chatroom — relay raw bytes with bouncesLeft decremented,
                                // preserving the original ciphertext so the intended recipients can decrypt.
                                size_t relayLen = 0;
                                if (LoraModule::Utilities::RelayMessage(buffer, len,
                                                                         _RelayBuffer, relayLen,
                                                                         routeBouncesLeft - 1))
                                {
                                    while (!_SendBufferIdle.load()) { vTaskDelay(pdMS_TO_TICKS(10)); }
                                    memcpy(_SendBuffer, _RelayBuffer, relayLen);
                                    _SendBufferLen = relayLen;
                                    _SendBufferIdle.store(false);
                                }
                            }
                            _lastReceivedMessages[routeSender] = routeMsgID;
                        }
                    }
                }
            }

            // Send any pending outbound message
            if (!_SendBufferIdle)
            {
                // Wait for a clear channel before transmitting
                while (_Driver->IsChannelBusy())
                {
                    ESP_LOGI(TAG, "Channel busy — waiting");
                    vTaskDelay(pdMS_TO_TICKS(AFTER_SEND_BLOCK_TIME_MS));
                }

                if (!_Driver->SendMessage(_SendBuffer, _SendBufferLen))
                {
                    ESP_LOGE(TAG, "Failed to send message");
                }
                _SendBufferIdle = true;
                vTaskDelay(pdMS_TO_TICKS(AFTER_SEND_BLOCK_TIME_MS));
            }

            // Apply a channel change requested from the settings task. Done here
            // rather than at the top of the loop so a packet that arrived on the
            // old channel is still processed, and so the StartReceiving() below
            // is what latches the new frequency.
            _ApplyPendingChannel();

            // Re-enter continuous receive mode for the next packet
            _Driver->StartReceiving();
        }
    }

    void SendQueueTask()
    {
        if (_sendQueue == nullptr)
        {
            vTaskDelete(NULL);
        }

        while (true)
        {
            // Block until a message is queued
            std::shared_ptr<LoraModule::LoraMessageInterface>* wrapper = nullptr;
            if (xQueueReceive(_sendQueue, &wrapper, portMAX_DELAY) != pdTRUE) { continue; }

            auto msg = *wrapper;
            delete wrapper;

            bool isOwn = (msg->sender == System_Utils::DeviceID);
            uint8_t attemptsLeft = isOwn
                ? std::max((uint8_t)1, LoraModule::Utilities::DefaultSendAttempts())
                : static_cast<uint8_t>(NUM_REBROADCAST_ATTEMPTS);

            ESP_LOGI(TAG, "Queued msgID 0x%08X sender 0x%08X — %s — attempts %d",
                     msg->msgID, msg->sender, isOwn ? "own" : "relay", attemptsLeft);

            while (attemptsLeft > 0)
            {
                // Random backoff in [MIN_SEND_DELAY_MS, MAX_SEND_DELAY_MS)
                uint32_t delayMs = MIN_SEND_DELAY_MS +
                    static_cast<uint32_t>(rand() % (MAX_SEND_DELAY_MS - MIN_SEND_DELAY_MS));
                vTaskDelay(pdMS_TO_TICKS(delayMs));

                // Wait for RadioTask to finish any in-progress send
                while (!_SendBufferIdle.load())
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                size_t outLen = 0;                

                if (LoraModule::Utilities::SerializeMessage(msg, _SendBuffer, outLen))
                {
                    _SendBufferLen = outLen;
                    _SendBufferIdle.store(false);

                    if (_ReceiveTaskHandle != nullptr)
                    {
                        xTaskNotifyGive(_ReceiveTaskHandle);
                    }

                    ESP_LOGI(TAG, "Transmitting msgID 0x%08X — attemptsLeft %d -> %d",
                             msg->msgID, attemptsLeft, attemptsLeft - 1);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to serialize msgID 0x%08X — dropping", msg->msgID);
                    break;
                }

                attemptsLeft--;
            }

            ESP_LOGI(TAG, "msgID 0x%08X — all attempts exhausted, removing", msg->msgID);
        }
    }

    void SetTaskHandles(TaskHandle_t sendHandle, TaskHandle_t receiveHandle)
    {
        _SendTaskHandle    = sendHandle;
        _ReceiveTaskHandle = receiveHandle;
    }

protected:
    // Retunes the radio if a channel change is pending. Radio task only —
    // it is the sole owner of the radio's registers and mode transitions.
    void _ApplyPendingChannel()
    {
        int channel = LoraModule::Utilities::TakePendingChannel();
        if (channel == 0) { return; }

        uint32_t hz = LoraModule::ChannelToHz(channel);
        ESP_LOGI(TAG, "Switching to channel %d (%u Hz)", channel, hz);

        _Driver->SetFrequency(hz);
        LoraModule::Utilities::ActiveChannel() = channel;
    }

    bool ShouldMessageBeForwarded(uint32_t senderID, uint32_t msgID, uint8_t bouncesLeft)
    {
        if (senderID == System_Utils::DeviceID) { return false; }
        if (bouncesLeft == 0) { return false; }

        auto it = _lastReceivedMessages.find(senderID);
        if (it == _lastReceivedMessages.end()) { return true; }
        return it->second != msgID;
    }

    LoraDriverInterface* _Driver;

    QueueHandle_t _sendQueue = nullptr;

    std::unordered_map<uint32_t, uint32_t> _lastReceivedMessages;

    TaskHandle_t _SendTaskHandle    = nullptr;
    TaskHandle_t _ReceiveTaskHandle = nullptr;

    std::atomic<bool> _SendBufferIdle { true };
    uint8_t  _SendBuffer[MAX_MESSAGE_SIZE]{};
    uint8_t  _RelayBuffer[MAX_MESSAGE_SIZE]{};
    size_t   _SendBufferLen = 0;
};

} // namespace LoraModule
