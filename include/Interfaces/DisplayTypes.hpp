#pragma once

// DisplayTypes.hpp
// Pure data types shared across the DisplayModule layer stack.
// No class dependencies, no FreeRTOS, no Adafruit — just structs and enums.
// Every layer (Interfaces, Utilities, HelperClasses) can include this safely.

#include <Adafruit_GFX.h>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // Debounce
    // -------------------------------------------------------------------------

    constexpr uint32_t DEBOUNCE_DELAY_MS = 100;

    // -------------------------------------------------------------------------
    // Canonical Input IDs
    // -------------------------------------------------------------------------
    // Reserved by the display module. All core windows use these IDs.
    // Consuming projects may define additional IDs starting at InputID::USER_BASE.
    //
    // Button roles:
    //   BUTTON_1 / BUTTON_2 — context-dependent function buttons
    //   BUTTON_3            — back / cancel
    //   BUTTON_4            — select / confirm
    //   ENC_UP / ENC_DOWN   — encoder scroll

    namespace InputID
    {
        // Core input IDs
        constexpr uint8_t BUTTON_1  = 1; 
        constexpr uint8_t BUTTON_2  = 2;
        constexpr uint8_t BUTTON_3  = 3; // back
        constexpr uint8_t BUTTON_4  = 4; // select
        constexpr uint8_t ENC_UP    = 5;
        constexpr uint8_t ENC_DOWN  = 6;
        // Not necessary for core functionality. Optional for power user features
        constexpr uint8_t ENC_BUTTON  = 7;
        // Projects may define IDs starting here
        constexpr uint8_t USER_BASE = 16;
    } // namespace InputID

    // -------------------------------------------------------------------------
    // Layer IDs
    // -------------------------------------------------------------------------
    // Rendered in ascending order (lower ID = drawn first).
    // The display module reserves CONTENT and WINDOW layers.
    // Application projects may define additional layers >= LayerID::USER_BASE.

    namespace LayerID
    {
        constexpr uint8_t CONTENT  = 0; // State draw commands (drawn first)
        constexpr uint8_t WINDOW   = 1; // Window chrome + input labels (drawn on top)

        // Application layers start here
        constexpr uint8_t USER_BASE = 8;
    } // namespace LayerID

    // -------------------------------------------------------------------------
    // InputContext
    // -------------------------------------------------------------------------
    // Passed to every onInputCommands handler and WindowInterface::handleInput.
    // Only Manager constructs this — add fields here to expand later.

    struct InputContext
    {
        uint8_t inputID = 0;
    };

    // -------------------------------------------------------------------------
    // InputEntry
    // -------------------------------------------------------------------------
    // Maps an input ID to a user-presentable label and two command vectors.
    // Defined here so WindowInterface can expose inputs() without depending on
    // the concrete Window class.

    struct InputEntry
    {
        std::string label;
        using CommandFn  = std::function<void(const InputContext &)>;
        using CommandVec = std::vector<CommandFn>;
        CommandVec onInputCommands;    // run on input, before render
        CommandVec onRenderedCommands; // run after render
    };

    // -------------------------------------------------------------------------
    // DrawContext
    // -------------------------------------------------------------------------
    // Passed to every draw command and layer. Provides display access and
    // dimensions so commands can adapt to any Adafruit_GFX-compatible display.

    struct DrawContext
    {
        Adafruit_GFX *display = nullptr;
        uint16_t      width   = 0;
        uint16_t      height  = 0;

        // Percentage-to-pixel helpers (0.0 – 1.0)
        int16_t toPixelX(float pct) const { return static_cast<int16_t>(width  * pct); }
        int16_t toPixelY(float pct) const { return static_cast<int16_t>(height * pct); }
    };

    // -------------------------------------------------------------------------
    // Display Command Queue types
    // -------------------------------------------------------------------------

    enum class CommandType : uint8_t
    {
        INPUT_COMMAND    = 0,
        CALLBACK_COMMAND = 1,
    };

    struct DisplayCommandQueueItem
    {
        CommandType commandType;

        union
        {
            struct { uint8_t  inputID;    } inputCommand;
            struct { uint32_t resourceID; } callbackCommand;
        } commandData;
    };

    // -------------------------------------------------------------------------
    // Text alignment helpers
    // -------------------------------------------------------------------------

    enum class TextAlignH : uint8_t { LEFT, CENTER, RIGHT };
    enum class TextAlignV : uint8_t { TOP, CENTER, BOTTOM, CONTENT_TOP, CONTENT_BOTTOM, LINE };

    struct TextFormat
    {
        TextAlignH hAlign       = TextAlignH::CENTER;
        TextAlignV vAlign       = TextAlignV::CENTER;
        uint8_t    line         = 1;
        int        distanceFrom = 0;

        TextFormat() = default;
        TextFormat(TextAlignH h, TextAlignV v, uint8_t ln = 1, int dist = 0)
            : hAlign(h), vAlign(v), line(ln), distanceFrom(dist) {}
    };

    struct TextDrawData
    {
        std::string text;
        TextFormat  format;

        TextDrawData() = default;
        TextDrawData(const char *txt)                   : text(txt)             {}
        TextDrawData(std::string txt)                   : text(std::move(txt))  {}
        TextDrawData(const char *txt,  TextFormat fmt)  : text(txt),            format(fmt) {}
        TextDrawData(std::string txt,  TextFormat fmt)  : text(std::move(txt)), format(fmt) {}
    };

} // namespace DisplayModule
