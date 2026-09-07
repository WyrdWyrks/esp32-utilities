#pragma once

#include <Arduino.h>
#include <MeshCore.h>            // mesh::RTCClock

#include "SystemUtilities.hpp"   // System_Utils / SystemModule::Utilities

namespace LoraModule
{
    // Bridges MeshCore's RTCClock abstraction onto the existing System_Utils
    // time-source registry (GPS today, more later). MeshCore uses
    // getCurrentTime() for packet freshness and getCurrentTimeUnique() for
    // id generation; it never needs a settable clock here because System_Utils
    // owns time in this codebase, so setCurrentTime() is intentionally a no-op.
    //
    // Before any source has a fix, getCurrentTime() falls back to a
    // millis()-derived monotonically increasing epoch so MeshCore's freshness
    // and uniqueness logic keeps working from cold boot.
    class MeshTimeClock : public mesh::RTCClock
    {
    public:
        uint32_t getCurrentTime() override
        {
            time_t utc = 0;
            if (System_Utils::GetCurrentUTC(utc) && utc > 0)
            {
                return static_cast<uint32_t>(utc);
            }
            return _FALLBACK_BASE + (millis() / 1000u);
        }

        void setCurrentTime(uint32_t /*epochSeconds*/) override
        {
            // System_Utils owns the clock; nothing to store here.
        }

    private:
        // 2025-01-01T00:00:00Z. Arbitrary — only the relative ordering of the
        // fallback timestamps matters until a real fix arrives.
        static constexpr uint32_t _FALLBACK_BASE = 1735689600u;
    };
}
