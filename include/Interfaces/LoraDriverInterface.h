#pragma once

#include <stddef.h>
#include <stdint.h>

class LoraDriverInterface
{
public:
    virtual bool Init() = 0;
    // Reads one packet into `buffer`, writing at most `capacity` bytes and
    // reporting the count in `outLen`. Implementations MUST NOT write past
    // `capacity` — the radio's byte count is live hardware state and can grow
    // mid-read when a second packet arrives, so it cannot bound the write.
    virtual bool ReceiveMessage(uint8_t* buffer, size_t capacity, size_t& outLen, size_t timeout) = 0;
    virtual bool SendMessage(const uint8_t* buffer, size_t len) = 0;
    // The callback runs in interrupt context and must do nothing but wake the
    // radio task. It is given no packet length: determining one requires SPI
    // register reads, which are not safe from an ISR and which race any read
    // already in progress on the radio task.
    virtual void RegisterOnReceive(void(*callback)()) = 0;
    virtual void StartReceiving() = 0;
    virtual int  PacketRssi() = 0;
    virtual bool IsChannelBusy() = 0;

    // Retunes the radio. Only ever called from the Manager's radio task, which
    // owns the SPI bus and the radio's mode transitions. Defaulted to a no-op
    // so drivers on a fixed frequency don't have to implement it.
    virtual void SetFrequency(uint32_t hz) {}

    virtual ~LoraDriverInterface() = default;
};
