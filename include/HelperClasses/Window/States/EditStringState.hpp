#pragma once

#include <string>
#include <cstddef>
#include <cstdio>
#include "EditStateBase.hpp"
#include "DisplayUtilities.hpp"
#include "TextDrawCommand.hpp"
#include <ArduinoJson.h>

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // EditStringState
    // -------------------------------------------------------------------------
    // Character-by-character string editor.
    //
    // The cursor position scrolls through a fixed set of legal characters.
    // The current character blinks on a 500 ms timer (alternating between
    // the selected char and an underscore).
    //
    // Button layout (mirrors the old Edit_String_State):
    //   ENC_UP / ENC_DOWN  — cycle through legal characters at cursor
    //   BUTTON_1           — delete last character        label: "Del"
    //   BUTTON_2           — confirm / exit with result   label: "Done"
    //   BUTTON_3           — cancel / exit without result label: "Back"
    //   BUTTON_4           — append current character     label: "Add"
    //
    // Payload in:
    //   {
    //     "cfgVal": "current text",   // optional; empty string if absent
    //     "maxLen": 16
    //   }
    //
    // Payload out (on BUTTON_2 confirm):
    //   { "return": "edited string" }
    // No payload on BUTTON_3 (cancel).
    //
    // refreshIntervalMs() returns BLINK_INTERVAL_MS so ContentLayer redraws
    // the blinking cursor automatically.  The owning Window must call
    // onTick() each time the refresh fires to toggle the cursor.

    class EditStringState : public EditStateBase
    {
    public:
        static constexpr uint32_t BLINK_INTERVAL_MS = 500;

        static constexpr const char* legalChars()
        {
            return  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz"
                    "0123456789"
                    "!@#$%^&*()-_=+[]{}|;:',.<>?/`~\"\\ ";
        }

        // Encoder click will jump to the next character in this set
        static constexpr const char* jumpChars()
        {
            return  "Aa0!";
        }

        EditStringState()
        {
            bindInput(InputID::BUTTON_1, "Del", [this](const InputContext &) {
                if (!_str.empty()) _str.pop_back();
                _rebuildDrawCommands();
            });
            wireConfirmInput(InputID::BUTTON_2, "Done");
            bindInput(InputID::BUTTON_3, "Back");
            bindInput(InputID::BUTTON_4, "Add", [this](const InputContext &) {
                if (_str.size() < _maxLen)
                    _str += legalChars()[_charPos];
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_UP, "", [this](const InputContext &) {
                _charPos = (_charPos == 0)
                           ? strlen(legalChars()) - 1
                           : _charPos - 1;
                _showCursor = false;
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_DOWN, "", [this](const InputContext &) {
                _charPos = (_charPos + 1) % strlen(legalChars());
                _showCursor = false;
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_BUTTON, "", [this](const InputContext &) {
                _jumpToNextChar();
                _showCursor = false;
                _rebuildDrawCommands();
            });
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &data) override
        {
            _str        = "";
            _maxLen     = 32;
            _charPos    = 0;     // index into LEGAL_CHARS
            _showCursor = true;

            if (data.payload)
            {
                auto &doc = *data.payload;
                if (!doc["maxLen"].isNull())
                    _maxLen = doc["maxLen"].as<size_t>();
                if (!doc["cfgVal"].isNull())
                    _str = doc["cfgVal"].as<std::string>();
            }

            // Clamp string to maxLen
            if (_str.size() > _maxLen)
                _str.resize(_maxLen);

            _rebuildDrawCommands();

            refreshIntervalMs = BLINK_INTERVAL_MS;
        }

        // ------------------------------------------------------------------
        // Tick — toggle cursor blink.
        // ------------------------------------------------------------------

        void onTick()
        {
            _showCursor = !_showCursor;
            _rebuildDrawCommands();
        }

        // ------------------------------------------------------------------
        // Result
        // ------------------------------------------------------------------

        const std::string &currentString() const { return _str; }

        std::shared_ptr<ArduinoJson::JsonDocument> buildResultPayload() const override
        {
            auto doc = std::make_shared<ArduinoJson::JsonDocument>();
            (*doc)["return"] = _str;
            return doc;
        }

        // ------------------------------------------------------------------
        // Input payload builder
        // ------------------------------------------------------------------

        static std::shared_ptr<ArduinoJson::JsonDocument>
        buildInputPayload(const std::string &currentValue, size_t maxLen)
        {
            auto doc = std::make_shared<ArduinoJson::JsonDocument>();
            (*doc)["cfgVal"] = currentValue;
            (*doc)["maxLen"] = maxLen;
            return doc;
        }

    private:
        std::string _str;
        size_t      _maxLen     = 32;
        size_t      _charPos    = 0;    // index into LEGAL_CHARS
        bool        _showCursor = true;

        // Advance _charPos to the next character (wrapping) that is a
        // member of jumpChars(). No-op if jumpChars() is empty.
        void _jumpToNextChar()
        {
            const char *legal = legalChars();
            const char *jump  = jumpChars();
            size_t legalLen = strlen(legal);
            size_t jumpLen  = strlen(jump);
            if (legalLen == 0 || jumpLen == 0) return;

            for (size_t offset = 1; offset <= legalLen; ++offset)
            {
                size_t pos = (_charPos + offset) % legalLen;
                if (strchr(jump, legal[pos]) != nullptr)
                {
                    _charPos = pos;
                    return;
                }
            }
        }

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            // Build display string: current text + cursor/char
            std::string display = _str;
            display += _showCursor ? '_' : legalChars()[_charPos];

            // String + cursor — line 3
            addDrawCommand(std::make_shared<TextDrawCommand>(
                display,
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 3 }
            ));

            // Length indicator — line 2  e.g. "(5/16)"
            char lenBuf[16];
            snprintf(lenBuf, sizeof(lenBuf),
                     "(%u/%u)",
                     static_cast<unsigned>(_str.size()),
                     static_cast<unsigned>(_maxLen));

            addDrawCommand(std::make_shared<TextDrawCommand>(
                std::string(lenBuf),
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 2 }
            ));
        }
    };

} // namespace DisplayModule
