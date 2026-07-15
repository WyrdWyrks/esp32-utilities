#pragma once

#include <NimBLEDevice.h>
#include <WiFi.h>
#include "ArduinoJson.h"
#include "RpcManager.h"

namespace BluetoothModule
{
    namespace detail
    {
        inline constexpr const char *DEGEN_SERVICE_UUID = "033c3d34-8405-46db-8326-07169d5353a9";
        inline constexpr const char *RPC_CHARACTERISTIC_UUID = "033c3d37-8405-46db-8326-07169d5353a9";

        inline constexpr size_t MAX_BLE_RPC_PACKET_SIZE = 1024 * 4;
        // 512 is the max BLE packet size, use 500 here to be safe.
        inline constexpr size_t MAX_OUTGOING_BLE_PACKET_SIZE = 500;

        inline bool gBluetoothConnected = false;
        inline int gBluetoothPin = 0;
        inline bool gBluetoothPaired = false;

        inline std::string &DeviceName()
        {
            static std::string device_name = "Beacon";
            return device_name;
        }

        class SystemBLEServer : public NimBLEServerCallbacks {
            void onConnect(BLEServer* pServer, NimBLEConnInfo& connInfo) override {
                // Require all connections to be paired.
                ESP_LOGI("SystemBLEServer", "Client connected, starting security");
                BLEDevice::startSecurity(connInfo.getConnHandle());
                gBluetoothConnected = true;
            }

            void onDisconnect(BLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
                // Start advertising again after the old client disconnects.
                BLEDevice::startAdvertising();
                gBluetoothConnected = false;
            }

            void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
                ESP_LOGI("SystemBLEServer", "Authentication complete for client");
                if (connInfo.isBonded()) {
                    gBluetoothPaired = true;
                } else {
                    gBluetoothPaired = false;
                }
            }

            uint32_t onPassKeyDisplay() override {
                uint32_t pass_key = random(100000, 999999);
                ESP_LOGI("SystemBLEServer", "Passkey for pairing: %06u", pass_key);
                gBluetoothPin = pass_key;
                return pass_key;
            }
        };

        class WifiNameCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
            void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
                std::string value = pCharacteristic->getValue();
            }
        };


        class WifiPassCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
            void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
                std::string value = pCharacteristic->getValue();
            }
        };

        class RpcCharacteristicCallbacks : public NimBLECharacteristicCallbacks {

        public:
            void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
                // RPC requests come in here.
                NimBLEAttValue data = pCharacteristic->getValue();
                uint32_t payload_size = data.length() - 1;
                ESP_LOGI("[BLE]", "Incoming RPC packet size: %u", payload_size);

                if (payload_size + _incomingPacketBufferIndex > MAX_BLE_RPC_PACKET_SIZE) {
                    // Overflow, reset buffer
                    _incomingPacketBufferIndex = 0;
                    ESP_LOGW("RpcCharacteristicCallbacks", "Incoming RPC packet overflow, resetting buffer");
                    return;
                }

                memcpy(&_incomingPacketBuffer[_incomingPacketBufferIndex], &data.c_str()[1], payload_size);
                _incomingPacketBufferIndex += payload_size;

                // First byte tells us if more data is coming.
                bool moreComing = data[0];
                if (!moreComing) {
                    processIncomingRpc();
                    _incomingPacketBufferIndex = 0;
                }
            }

            void processIncomingRpc() {
                // Process the packet
                JsonDocument doc;
                DeserializationError error = deserializeMsgPack(doc, _incomingPacketBuffer, _incomingPacketBufferIndex);
                if (error) {
                    ESP_LOGW("[BLE]", "Failed to deserialize incoming RPC packet");
                    return;
                }

                std::string debugStr;
                serializeJson(doc, debugStr);
                ESP_LOGI("[BLE]", "Received RPC request: %s", debugStr.c_str());

                auto returnCode = RpcModule::Utilities::CallRpc(doc[RpcModule::Utilities::RPC_FUNCTION_NAME_FIELD()].as<std::string>(), doc);

                if (returnCode != RpcModule::RpcReturnCode::RPC_SUCCESS)
                {
                    ESP_LOGW("RpcCharacteristicCallbacks", "RPC function returned code %d", returnCode);
                }
                else
                {
                    ESP_LOGV("RpcCharacteristicCallbacks", "RPC function returned code %d", returnCode);
                }

                size_t packedSize = measureMsgPack(doc);
                if (packedSize > MAX_BLE_RPC_PACKET_SIZE) {
                    ESP_LOGW("[BLE]", "Outgoing RPC packet too large");
                    return;
                }
                _outgoingPacketBufferIndex = 0;
                _outgoingPacketSize = packedSize;

                debugStr.clear();
                serializeJson(doc, debugStr);
                ESP_LOGI("[BLE]", "Sending RPC response: %s", debugStr.c_str());

                serializeMsgPack(doc, _outgoingPacketBuffer, packedSize);
            }

            void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
                // Responses to RPCs go here.
                if (_outgoingPacketBufferIndex >= _outgoingPacketSize) {
                    // No more data to send.
                    pCharacteristic->setValue(_outgoingPacketBuffer, 0);
                    return;
                }
                uint32_t bytesRemaining = _outgoingPacketSize - _outgoingPacketBufferIndex;

                bool moreComing = bytesRemaining > MAX_OUTGOING_BLE_PACKET_SIZE;
                uint32_t chunkSize = moreComing ? MAX_OUTGOING_BLE_PACKET_SIZE : bytesRemaining;

                uint8_t characteristic_buffer[MAX_OUTGOING_BLE_PACKET_SIZE + 1];
                // Set first byte to indicate if more data is coming.
                characteristic_buffer[0] = moreComing ? 1 : 0;
                memcpy(&characteristic_buffer[1], &_outgoingPacketBuffer[_outgoingPacketBufferIndex], chunkSize);

                pCharacteristic->setValue(characteristic_buffer, chunkSize + 1);
                _outgoingPacketBufferIndex += chunkSize;
            }

            uint8_t _incomingPacketBuffer[MAX_BLE_RPC_PACKET_SIZE];
            uint32_t _incomingPacketBufferIndex = 0;

            uint8_t _outgoingPacketBuffer[MAX_BLE_RPC_PACKET_SIZE];
            uint32_t _outgoingPacketBufferIndex = 0;
            uint32_t _outgoingPacketSize = 0;
        };
    } // namespace detail
} // namespace BluetoothModule

class BluetoothUtilities {
public:
    static void initBluetooth()
    {
        using namespace BluetoothModule::detail;

        // Can't have WiFi and BT active
        WiFi.disconnect(true);  // Disconnect from the network
        WiFi.mode(WIFI_OFF);    // Switch WiFi off

        ESP_LOGI("BluetoothUtilities", "Initializing Bluetooth as %s...", _DeviceName().c_str());

        BLEDevice::init(_DeviceName());
        NimBLEDevice::setMTU(BLE_ATT_MTU_MAX); // Use maximum MTU for largest packets.

        // -- Set security parameters
        // Require pairing (bonding) and SC for secure connection pairing.
        BLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/true);
        // State that we can display a PIN code for pairing, but no input supported.
        BLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

        BLEServer* pServer = BLEDevice::createServer();
        pServer->setCallbacks(new SystemBLEServer());

        BLEService* pService = pServer->createService(DEGEN_SERVICE_UUID);

        BLECharacteristic* pRpcCharacteristic = pService->createCharacteristic(
            RPC_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::READ |
            NIMBLE_PROPERTY::WRITE |
            NIMBLE_PROPERTY::READ_ENC |
            NIMBLE_PROPERTY::WRITE_ENC |
            NIMBLE_PROPERTY::READ_AUTHEN |
            NIMBLE_PROPERTY::WRITE_AUTHEN
        );
        pRpcCharacteristic->setCallbacks(new RpcCharacteristicCallbacks());

        // NimBLEService::start() is deprecated and has no effect; services are
        // started automatically when the server starts advertising.

        BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
        pAdvertising->setName(_DeviceName().c_str());
        pAdvertising->addServiceUUID(DEGEN_SERVICE_UUID);
        // Show up with a watch icon lol.
        constexpr uint16_t BLE_APPEARANCE_GENERIC_WATCH = 192;
        pAdvertising->setAppearance(BLE_APPEARANCE_GENERIC_WATCH);

        BLEDevice::startAdvertising();
    }

    static bool bluetoothConnected()
    {
        return BluetoothModule::detail::gBluetoothConnected;
    }

    static bool bluetoothPaired()
    {
        return BluetoothModule::detail::gBluetoothPaired;
    }

    static int bluetoothPin()
    {
        return BluetoothModule::detail::gBluetoothPin;
    }

    static void SettingsUpdated(JsonDocument &doc)
    {
        // This function is called when the settings file is updated. We can use it to update any relevant Bluetooth settings.
        if (!doc["Device Name"].isNull()) {
            _DeviceName() = doc["Device Name"].as<std::string>();
            BLEDevice::setDeviceName(_DeviceName());
        }
    }

private:
    static std::string &_DeviceName()
    {
        return BluetoothModule::detail::DeviceName();
    }
};

// Backwards-compatible alias for the previous class name.
using Bluetooth_Utils = BluetoothUtilities;
