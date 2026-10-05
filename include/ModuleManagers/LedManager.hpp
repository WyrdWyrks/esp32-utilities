#pragma once

#include <FastLED.h>
#include "LedUtilities.hpp"
#include "SystemUtilities.hpp"

#if HARDWARE_VERSION < 3
#define BUZZER_PIN 4
#else
#define BUZZER_PIN 7
#define HAPTIC_VIBRATION_PIN 6
#endif

namespace UxModule
{
    // LED animation/rendering lives in the LedPatternInterface registry driven by
    // UxModule::LedUtilities (RingPoint, RingPulse, SolidRing, ScrollWheel,
    // Flashlight, ...). The manager bootstraps FastLED + the pattern task and owns
    // the discrete hardware outputs (buzzer, haptic motor).
    class LedManager
    {
    public:
        static constexpr size_t LED_MS_PER_FRAME = 50;

        // ledBuffer must be the buffer handed to FastLED.addLeds(). Patterns may
        // be registered before or after this call.
        static void init(size_t numLeds, CRGB *ledBuffer, uint8_t cpuCore)
        {
            ESP_LOGI(TAG, "init: %u LEDs, pattern task on core %u",
                     static_cast<unsigned>(numLeds), static_cast<unsigned>(cpuCore));

            FastLED.setBrightness(255);
            FastLED.clear();
            FastLED.show();

            LedUtilities::attachBuffer(ledBuffer, numLeds);
            LedUtilities::setTickRate(LED_MS_PER_FRAME);
            LedPatternInterface::SetThemeColor(LedUtilities::ThemeColor());

            // From here on only the LED task writes the strip or calls show().
            int patternTaskID = System_Utils::registerTask(LedUtilities::iteratePatterns, "LED Task", 4096, NULL, 1, cpuCore);
            LedUtilities::SetIteratePatternTaskHandle(System_Utils::getTask(patternTaskID));
            LedUtilities::refresh();
        }

        static void buzzerNotification(uint16_t frequency = 1000, size_t duration = 100)
        {
            tone(BUZZER_PIN, frequency, duration);
        }

        static void applyHapticFeedback(uint8_t intensity)
        {
#if HARDWARE_VERSION >= 3
            static constexpr TickType_t HAPTIC_MS = 80;

            // Constant drive for HAPTIC_MS, then off — same strong pulse as the old
            // blocking delay(80), but the turn-off is scheduled on a one-shot timer so
            // the caller (the render loop) keeps running instead of stalling 80 ms per
            // input. Re-firing mid-pulse just restarts the window from the latest input.
            analogWrite(HAPTIC_VIBRATION_PIN, intensity);

            if (hapticTimer == nullptr)
            {
                hapticTimer = xTimerCreateStatic(
                    "haptic", HAPTIC_MS / portTICK_PERIOD_MS, pdFALSE, nullptr,
                    hapticOff, &hapticTimerBuffer);
            }
            xTimerReset(hapticTimer, 0);
#endif
        }

    private:
        static constexpr const char *TAG = "LedManager";

        // One-shot timer that switches the haptic motor off, so applyHapticFeedback()
        // never blocks the calling task (e.g. the display/render loop).
        inline static TimerHandle_t hapticTimer = nullptr;
        inline static StaticTimer_t hapticTimerBuffer;

        // Timer callback: runs in the FreeRTOS timer service (Tmr Svc) daemon, not the
        // caller's task, so turning the motor off costs the display/render loop nothing.
        static void hapticOff(TimerHandle_t)
        {
#if HARDWARE_VERSION >= 3
            analogWrite(HAPTIC_VIBRATION_PIN, 0);
#endif
        }
    };
}
