#pragma once

#include "FastLED.h"
#include "ArduinoJson.h"
#include "LedSegment.hpp"

namespace UxModule
{
    class LedPatternInterface
    {
    public:
        LedPatternInterface() {
            currTick = 0;
            animationTicks = 0;
            animationMS = 0;
            startTime = 0;
        }

        LedPatternInterface(LedSegment segment)
            : _segment(std::move(segment))
        {
            currTick = 0;
            animationTicks = 0;
            animationMS = 0;
            startTime = 0;
        }

        virtual ~LedPatternInterface() = default;

        // Used to pass custom parameters to the pattern
        // This function will typically follow the pattern of:
        // if (!config["key"].isNull()) { key = config["key"]; }
        virtual void configurePattern(JsonDocument &config) {}

        // Called to reset the pattern to its initial state
        void resetPattern() {
            currTick = 0;
            startTime = 0;
        }

        void setAnimationLengthMS(size_t ms) {
            animationTicks = ms / _MsPerTick();
            animationMS = ms;
        }

        void setAnimationLengthTicks(size_t ticks) {
            animationTicks = ticks;
            animationMS = ticks * _MsPerTick();
        }

        static void setTickRate(size_t ms) {
            _MsPerTick() = ms;
        }

        // Called to iterate a single frame of the animation
        // Some animations only have one frame and will only be called here
        // Returns true if the last frame of the loop has played
        virtual bool iterateFrame() { return true; }

        // Whether the pattern currently claims the LEDs of its segment. The LED
        // task composites the layers of enabled patterns in priority order, and
        // only an active pattern hides the ones below it. Patterns that are
        // "off" between animations (a finished button flash, a flashlight that
        // is switched off) return false so lower-priority layers show through
        // instead of being covered with black.
        virtual bool isActive() const { return true; }

        // Points the pattern's segment at its private layer (same layout as the
        // strip). Called by UxModule::LedUtilities at registration; patterns never
        // draw into the strip buffer directly.
        void BindLayer(CRGB *layer) { _segment.rebase(layer); }

        const LedSegment &Segment() const { return _segment; }

        // Plays a pattern to completion synchronously, blocking the caller. Used for
        // sequences that must finish before the asynchronous LED task can run them --
        // e.g. a shutdown animation that plays right before power is cut.
        // Call it through UxModule::LedUtilities::playBlocking(), which holds the LED
        // lock so the LED task cannot interleave its own frames. The pattern must
        // not be registered: it draws straight into the strip buffer.
        static void PlayBlocking(LedPatternInterface &pattern)
        {
            pattern.resetPattern();
            while (!pattern.iterateFrame())
            {
                FastLED.show();
                delay(_MsPerTick());
            }
            FastLED.show();
        }

        // Clears the segment and resets animation state
        virtual void clearPattern() {
            _segment.clear();
            resetPattern();
        }

        // Every pattern should have a static member holding the registered pattern id
        // This is so different modules can use the same pattern without re-registering it
        virtual void SetRegisteredPatternID(int patternID) = 0;

        static void SetThemeColor(CRGB color) {
            ThemeColor() = color;
        }

        static CRGB &ThemeColor()
        {
            static CRGB themeColor = CRGB(0, 0, 0);
            return themeColor;
        }

    protected:
        LedSegment _segment;

        static size_t &_MsPerTick()
        {
            static size_t msPerTick = 15;
            return msPerTick;
        }

        // Length of an animation loop
        size_t animationTicks;
        size_t animationMS;

        // Current progress of animation
        size_t currTick;

        // Timestamp an animation was started
        size_t startTime;

    };
}
