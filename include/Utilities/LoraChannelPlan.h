#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>

namespace LoraModule
{
    // -------------------------------------------------------------------------
    // US 902-928 MHz channel plan
    // -------------------------------------------------------------------------
    // Frequency is exposed to the user as a walkie-talkie style channel number
    // instead of a raw MHz value. Everything that needs a frequency derives it
    // from here.
    //
    // The grid is only valid at the 500 kHz signal bandwidth the radio runs at
    // (BootstrapLora calls SetSignalBandwidth(500E3)). 1.5 MHz spacing leaves
    // 3x the occupied bandwidth between neighbours, so adjacent channels don't
    // hear each other. If the bandwidth ever becomes user-configurable this
    // table has to be recomputed — 200 kHz-style spacing, which the old
    // "Frequency" setting used, overlaps almost completely at 500 kHz.
    //
    // The top channel is exactly 915.0 MHz, the frequency every build before
    // channel support hard-coded, so a device left on the default still talks
    // to anything running older firmware.
    //
    // Occupied edges land at 904.25 and 915.25 MHz: inside the 902-928 ISM band
    // with margin at both ends, and clear of the 923-928 MHz region used for
    // LoRaWAN downlinks.
    //
    //   Ch 1  904.5      Ch 5  910.5
    //   Ch 2  906.0      Ch 6  912.0
    //   Ch 3  907.5      Ch 7  913.5
    //   Ch 4  909.0      Ch 8  915.0  (default)

    inline constexpr uint32_t LORA_CHANNEL_BASE_HZ    = 904500000u;  // channel 1
    inline constexpr uint32_t LORA_CHANNEL_SPACING_HZ = 1500000u;    // 1.5 MHz
    inline constexpr int      LORA_CHANNEL_COUNT      = 8;
    inline constexpr int      LORA_CHANNEL_DEFAULT    = 8;           // 915.0 MHz

    // Channels are 1-based so the number in code matches the number on screen.
    inline constexpr bool IsValidChannel(int channel)
    {
        return channel >= 1 && channel <= LORA_CHANNEL_COUNT;
    }

    // Out-of-range channels fall back to the default rather than retuning the
    // radio somewhere unpredictable.
    inline constexpr uint32_t ChannelToHz(int channel)
    {
        const int ch = IsValidChannel(channel) ? channel : LORA_CHANNEL_DEFAULT;
        return LORA_CHANNEL_BASE_HZ
             + static_cast<uint32_t>(ch - 1) * LORA_CHANNEL_SPACING_HZ;
    }

    // "8 - 915.0 MHz", for the settings menu. Integer math so this stays off
    // the FPU and prints exactly.
    inline std::string ChannelLabel(int channel)
    {
        const uint32_t hz = ChannelToHz(channel);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d - %u.%u MHz",
                 channel,
                 static_cast<unsigned>(hz / 1000000u),
                 static_cast<unsigned>((hz % 1000000u) / 100000u));
        return buf;
    }

} // namespace LoraModule
