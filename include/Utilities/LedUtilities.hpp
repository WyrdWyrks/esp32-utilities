#pragma once

#include <algorithm>
#include <map>
#include <vector>
#include "ArduinoJson.h"
#include "FastLED.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "SystemUtilities.hpp"
#include "LedPatternInterface.hpp"

namespace UxModule
{
    // Draw priority for patterns whose segments share LEDs. Each LED shows the
    // highest-priority enabled pattern that is currently active (see
    // LedPatternInterface::isActive) and covers it; equal priorities fall back
    // to registration order (later wins). Any int works, these are the tiers
    // the stock patterns use.
    struct LedPriority
    {
        static constexpr int BACKGROUND = 0;   // screen indicators: ring pointer, scroll wheel, pulses
        static constexpr int FEEDBACK   = 10;  // transient feedback: button flash, trace flows
        static constexpr int OVERLAY    = 50;  // covers everything below while on: flashlight
        static constexpr int MODAL      = 100; // must stay readable over everything: lock screen guide
    };

    // Registry entry for one pattern.
    struct LedPatternStatus
    {
        int animationID = -1;
        LedPatternInterface *pattern = nullptr;
        int loopsRemaining = 0;   // -1 = forever, 0 = idle, n = loops left
        bool enabled = false;
        int priority = LedPriority::BACKGROUND;
        bool iterateOnce = false; // one frame requested by iteratePattern()
        std::vector<CRGB> layer;  // the pattern's private copy of the strip
    };

    // LED pattern registry and the LED task.
    //
    // Ownership: only the LED task (iteratePatterns) writes the strip buffer or
    // calls FastLED.show(). Every pattern draws into its own layer; the task
    // composites the layers by priority into the strip and pushes it. All other
    // tasks (display, mesh, RPC) only call the methods below, which take a
    // recursive lock and wake the task. Patterns must never call FastLED.show().
    class LedUtilities
    {
    public:
        // RAII hold of the LED lock. Recursive, so a locked caller can call any
        // method of this class.
        class LockGuard
        {
        public:
            LockGuard() { xSemaphoreTakeRecursive(_Lock(), portMAX_DELAY); }
            ~LockGuard() { xSemaphoreGiveRecursive(_Lock()); }
            LockGuard(const LockGuard &) = delete;
            LockGuard &operator=(const LockGuard &) = delete;
        };

        // Gives every registered pattern (and every later one) a layer the size
        // of the strip. Called once by UxModule::LedManager::init.
        static void attachBuffer(CRGB *strip, size_t length)
        {
            LockGuard lock;
            _Strip = strip;
            _StripLength = length;
            for (auto &kv : _Patterns)
            {
                _BindLayer(kv.second);
            }
        }

        // Registers a pattern and returns the pattern ID
        static int registerPattern(LedPatternInterface *pattern, int priority = LedPriority::BACKGROUND)
        {
            if (pattern == nullptr)
            {
                ESP_LOGW(TAG, "registerPattern: Attempted to register null pattern");
                return -1;
            }

            LockGuard lock;
            int id = _NextPatternID++;
            auto &status = _Patterns[id];
            status.animationID = id;
            status.pattern = pattern;
            status.priority = priority;
            pattern->SetRegisteredPatternID(id);
            _BindLayer(status);
            _RebuildDrawOrder();
            return id;
        }

        // Unregisters a pattern and deletes it
        static void unregisterPattern(int patternID)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            delete status->pattern;
            _Patterns.erase(patternID);
            _RebuildDrawOrder();
            _RequestShow();
        }

        // Changes the draw priority of a registered pattern
        static void setPatternPriority(int patternID, int priority)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            status->priority = priority;
            _RebuildDrawOrder();
            _RequestShow();
        }

        // Resets a pattern to its initial state
        static void resetPattern(int patternID)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status != nullptr)
            {
                status->pattern->resetPattern();
            }
        }

        // Sets a pattern's animation length in ms
        static void setAnimationLengthMS(int patternID, size_t ms)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status != nullptr)
            {
                status->pattern->setAnimationLengthMS(ms);
            }
        }

        // Sets a pattern's animation length in ticks
        static void setAnimationLengthTicks(int patternID, size_t ticks)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status != nullptr)
            {
                status->pattern->setAnimationLengthTicks(ticks);
            }
        }

        // Sends a configuration object to a pattern. Applies whether or not the
        // pattern is enabled: a pattern only ever touches its own layer, so a
        // state can configure first and enable after.
        static void configurePattern(int patternID, JsonDocument &config)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                ESP_LOGW(TAG, "configurePattern: Attempted to configure non-existent pattern with ID %d", patternID);
                return;
            }

            status->pattern->configurePattern(config);
        }

        // Sets a pattern to loop n times, or indefinitely if numLoops is -1
        static void loopPattern(int patternID, int numLoops)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            status->loopsRemaining = numLoops;
            if (numLoops != 0)
            {
                _Notify();
            }
        }

        // Enables a pattern. Its layer joins the composite on the next frame.
        static void enablePattern(int patternID)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                ESP_LOGW(TAG, "enablePattern: Attempted to enable non-existent pattern with ID %d", patternID);
                return;
            }

            status->enabled = true;
            _RequestShow();
        }

        // Disables a pattern and clears its layer, so a later enable never shows
        // stale content.
        static void disablePattern(int patternID)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            status->enabled = false;
            clearPattern(patternID);
        }

        // Clears the given pattern's layer, stops it looping and drops any
        // pending single frame. The LED task pushes the result.
        static void clearPattern(int patternID)
        {
            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            status->pattern->clearPattern();
            status->loopsRemaining = 0;
            status->iterateOnce = false;
            _RequestShow();
        }

        // Requests one frame of a pattern. A request made while the pattern is
        // disabled is kept and drawn once it is enabled, so configure ->
        // iterate -> enable shows the new configuration on the first frame.
        static void iteratePattern(int patternID)
        {
            if (patternID < 0)
            {
                ESP_LOGW(TAG, "iteratePattern: Cannot iterate pattern because pattern ID is invalid: %d", patternID);
                return;
            }

            LockGuard lock;
            auto status = _Find(patternID);
            if (status == nullptr)
            {
                return;
            }

            status->iterateOnce = true;
            _Notify();
        }

        // Re-composites and pushes the strip without changing any pattern, e.g.
        // after the LED supply was cut and restored.
        static void refresh()
        {
            LockGuard lock;
            _RequestShow();
        }

        // Plays an unregistered pattern to completion on the caller's task while
        // holding the LED lock, so the LED task cannot interleave frames. Only
        // for the shutdown fade: anything else that calls into this class
        // (e.g. a trace flow from the mesh task) waits until it finishes.
        static void playBlocking(LedPatternInterface &pattern)
        {
            LockGuard lock;
            LedPatternInterface::PlayBlocking(pattern);
        }

        // Sets the tick rate for looping LED patterns
        static void setTickRate(size_t ms)
        {
            LockGuard lock;
            LedPatternInterface::setTickRate(ms);
            _PatternTickRateMS = ms;
        }

        // Sets the user's theme color
        // This is used by patterns that don't have their own color configuration
        static void setThemeColor(uint8_t r, uint8_t g, uint8_t b)
        {
            setThemeColor(CRGB(r, g, b));
        }

        static void setThemeColor(CRGB color)
        {
            LockGuard lock;
            _ThemeColor = color;
            LedPatternInterface::SetThemeColor(_ThemeColor);
        }

        static CRGB &ThemeColor() { return _ThemeColor; }

        static void SetIteratePatternTaskHandle(TaskHandle_t handle) { _TaskHandle = handle; }

        // The LED task. Iterates patterns into their layers, composites the
        // layers into the strip by priority and pushes it, then sleeps until the
        // next tick (while anything is looping) or the next request.
        static void iteratePatterns(void *pvParameters)
        {
            const TickType_t minGap = std::max<TickType_t>(1, pdMS_TO_TICKS(MIN_SHOW_GAP_MS));
            TickType_t lastShow = 0;
            bool hasShown = false;
            bool resendPending = false;

            while (true)
            {
                // Spacing is enforced before taking the lock, so callers on other
                // tasks never wait on it.
                if (hasShown)
                {
                    TickType_t elapsed = xTaskGetTickCount() - lastShow;
                    if (elapsed < minGap)
                    {
                        vTaskDelay(minGap - elapsed);
                    }
                }

                bool workToDo = false;
                TickType_t tick = pdMS_TO_TICKS(_PatternTickRateMS);
                {
                    LockGuard lock;
                    bool rendered = false;

                    for (auto &kv : _Patterns)
                    {
                        auto &status = kv.second;
                        bool looping = status.loopsRemaining != 0;

                        // A frame requested while disabled stays pending.
                        if (!status.enabled || (!looping && !status.iterateOnce))
                        {
                            continue;
                        }

                        bool loopDone = status.pattern->iterateFrame();
                        status.iterateOnce = false;
                        rendered = true;

                        if (looping && loopDone && status.loopsRemaining > 0)
                        {
                            status.loopsRemaining--;
                        }

                        if (status.loopsRemaining != 0)
                        {
                            workToDo = true;
                        }
                    }

                    if (rendered || _ShowRequested)
                    {
                        _ShowRequested = false;
                        _Composite();
                        FastLED.show();
                        lastShow = xTaskGetTickCount();
                        hasShown = true;
                        resendPending = true;
                    }
                }

                TickType_t wait = portMAX_DELAY;
                if (workToDo)
                {
                    wait = std::max<TickType_t>(1, tick);
                }
                else if (resendPending)
                {
                    wait = pdMS_TO_TICKS(IDLE_RESEND_MS);
                }

                if (ulTaskNotifyTake(pdTRUE, wait) == 0 && !workToDo && resendPending)
                {
                    // The strip went idle with no new request. Push the same
                    // frame once more: a frame corrupted in transit (an RMT
                    // refill interrupt held off, e.g. by a flash write) would
                    // otherwise stay on the LEDs until the next change.
                    LockGuard lock;
                    FastLED.show();
                    lastShow = xTaskGetTickCount();
                    resendPending = false;
                }
            }
        }

    protected:
        static constexpr const char *TAG = "LedUtilities";

        // FastLED's RMT5 driver copies the strip into its own buffer before it
        // waits for the previous transmit to finish, so a second show() inside
        // one frame time corrupts the tail of the first. One frame of 61
        // WS2812B LEDs is 61 x 24 bits x 1.25 us + 280 us reset ~= 2.1 ms;
        // 4 ms leaves margin.
        static constexpr uint32_t MIN_SHOW_GAP_MS = 4;

        // Delay after the last change before the one confirming re-send of an
        // idle strip. Not periodic: one re-send per burst of changes.
        static constexpr uint32_t IDLE_RESEND_MS = 250;

        inline static CRGB _ThemeColor;
        inline static std::map<int, LedPatternStatus> _Patterns;
        inline static std::vector<LedPatternStatus *> _DrawOrder;
        inline static int _NextPatternID = 0;
        inline static size_t _PatternTickRateMS = 50;
        inline static TaskHandle_t _TaskHandle = nullptr;
        inline static bool _ShowRequested = false;
        inline static CRGB *_Strip = nullptr;
        inline static size_t _StripLength = 0;

        static SemaphoreHandle_t _Lock()
        {
            static SemaphoreHandle_t lock = xSemaphoreCreateRecursiveMutex();
            return lock;
        }

        static LedPatternStatus *_Find(int patternID)
        {
            auto it = _Patterns.find(patternID);
            return (it == _Patterns.end()) ? nullptr : &it->second;
        }

        static void _Notify()
        {
            if (_TaskHandle != nullptr)
            {
                xTaskNotifyGive(_TaskHandle);
            }
        }

        static void _RequestShow()
        {
            _ShowRequested = true;
            _Notify();
        }

        static void _BindLayer(LedPatternStatus &status)
        {
            if (_Strip == nullptr)
            {
                return;
            }

            const LedSegment &segment = status.pattern->Segment();
            size_t size = _StripLength;
            if (segment.length() > 0 && segment.maxPhysicalIndex() >= _StripLength)
            {
                ESP_LOGE(TAG, "Pattern %d reaches LED %u but the strip has %u; LEDs past the end are never shown",
                         status.animationID, static_cast<unsigned>(segment.maxPhysicalIndex()),
                         static_cast<unsigned>(_StripLength));
                size = segment.maxPhysicalIndex() + 1;
            }

            status.layer.assign(size, CRGB::Black);
            status.pattern->BindLayer(status.layer.data());
        }

        // Lowest priority first so higher priorities overwrite; std::map order
        // makes registration order the tie-break.
        static void _RebuildDrawOrder()
        {
            _DrawOrder.clear();
            for (auto &kv : _Patterns)
            {
                _DrawOrder.push_back(&kv.second);
            }
            std::stable_sort(_DrawOrder.begin(), _DrawOrder.end(),
                             [](const LedPatternStatus *a, const LedPatternStatus *b) {
                                 return a->priority < b->priority;
                             });
        }

        // LEDs that no active pattern covers end up dark.
        static void _Composite()
        {
            if (_Strip == nullptr)
            {
                return;
            }

            std::fill(_Strip, _Strip + _StripLength, CRGB::Black);

            for (auto status : _DrawOrder)
            {
                if (!status->enabled || status->layer.empty() || !status->pattern->isActive())
                {
                    continue;
                }

                const LedSegment &segment = status->pattern->Segment();
                for (size_t i = 0; i < segment.length(); i++)
                {
                    size_t led = segment.physicalIndex(i);
                    if (led < _StripLength)
                    {
                        _Strip[led] = status->layer[led];
                    }
                }
            }
        }
    };
}
