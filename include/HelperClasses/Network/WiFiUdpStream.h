#pragma once

#include "NetworkStreamInterface.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#define LOG_TAG __FILE__

namespace ConnectivityModule
{

    class WiFiUdpStream : public NetworkStreamInterface
    {
    public:
        WiFiUdpStream() : NetworkStreamInterface() {}
        WiFiUdpStream(uint16_t port) : NetworkStreamInterface(port) {}

        ~WiFiUdpStream() { 
            _udp.stop();
        }

        Stream &GetStream() { return _udp; }

        bool BeginPacket() { return _udp.beginPacket(_remoteIP, _remotePort) == 1; }

        bool EndPacket() { return _udp.endPacket() == 1; }

        void Flush() { _udp.clear(); }

        bool EstablishConnection(IPAddress ip, uint16_t port)
        {
            ESP_LOGI(TAG, "Connecting to: %s:%d", ip.toString().c_str(), port);
            _remoteIP = ip;
            _remotePort = port;
            return _udp.begin(WiFi.localIP(), port) == 1;
        }

    protected:
        WiFiUDP _udp;

        IPAddress _remoteIP = IPAddress(0, 0, 0, 0);
        uint16_t _remotePort = 0;
    };
}
