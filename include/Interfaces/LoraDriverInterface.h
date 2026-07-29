#pragma once

#include <stddef.h>
#include <stdint.h>

class LoraDriverInterface
{
public:
    virtual bool Init() = 0;
    virtual bool ReceiveMessage(uint8_t* buffer, size_t& outLen, size_t timeout) = 0;
    virtual bool SendMessage(const uint8_t* buffer, size_t len) = 0;
    virtual void RegisterOnReceive(void(*callback)(int)) = 0;
    virtual void StartReceiving() = 0;
    virtual int  PacketRssi() = 0;
    virtual bool IsChannelBusy() = 0;

    // Retunes the radio. Only ever called from the Manager's radio task, which
    // owns the SPI bus and the radio's mode transitions. Defaulted to a no-op
    // so drivers on a fixed frequency don't have to implement it.
    virtual void SetFrequency(uint32_t hz) {}

    virtual ~LoraDriverInterface() = default;
};
