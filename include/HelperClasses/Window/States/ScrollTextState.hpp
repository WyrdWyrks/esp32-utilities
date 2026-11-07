#pragma once

#include <string>
#include <vector>
#include "WindowState.hpp"
#include "DisplayUtilities.hpp"
#include "TextDrawCommand.hpp"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // ScrollTextState
    // -------------------------------------------------------------------------
    // Word-wrapped block of text scrolled with the encoder. '\n' forces a break
    // ("\n\n" leaves a blank line). The first and last text lines are left free
    // for the corner button labels; the ^/v hints only show when there is more
    // text in that direction.
    //
    // Button actions are left to the owning Window:
    //   auto page = std::make_shared<ScrollTextState>("Long text...");
    //   page->bindInput(InputID::BUTTON_4, "Next", [this](auto &) { ... });

    class ScrollTextState : public WindowState
    {
    public:
        explicit ScrollTextState(std::string text = "") : _text(std::move(text))
        {
            bindInput(InputID::ENC_UP, "", [this](const InputContext &) {
                if (_top > 0) { --_top; _rebuild(); }
            });
            bindInput(InputID::ENC_DOWN, "", [this](const InputContext &) {
                if (_top + _visibleLines() < _lines.size()) { ++_top; _rebuild(); }
            });
        }

        void setText(std::string text) { _text = std::move(text); }

        void onEnter(const StateTransferData &) override
        {
            _lines = wrap(_text, Utilities::drawContext().width / CHAR_W);
            _top = 0;
            _rebuild();
        }

        // Greedy word wrap; words longer than a line are hard-split.
        static std::vector<std::string> wrap(const std::string &text, size_t width)
        {
            std::vector<std::string> out;
            if (width == 0) return out;

            size_t start = 0;
            while (start <= text.size())
            {
                size_t nl = text.find('\n', start);
                std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);

                std::string line;
                size_t i = 0;
                while (i < para.size())
                {
                    size_t sp = para.find(' ', i);
                    std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                    i = (sp == std::string::npos) ? para.size() : sp + 1;

                    while (word.size() > width)
                    {
                        if (!line.empty()) { out.push_back(line); line.clear(); }
                        out.push_back(word.substr(0, width));
                        word.erase(0, width);
                    }

                    if (line.empty())                              line = word;
                    else if (line.size() + 1 + word.size() <= width) line += " " + word;
                    else                                           { out.push_back(line); line = word; }
                }
                out.push_back(line);

                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            return out;
        }

    private:
        static constexpr uint16_t CHAR_W = 6; // default GFX font at text size 1

        std::string              _text;
        std::vector<std::string> _lines;
        size_t                   _top = 0;

        size_t _visibleLines() const
        {
            auto bottom = Utilities::selectBottomTextLine();
            return bottom > 2 ? bottom - 2 : 1;
        }

        void _rebuild()
        {
            clearDrawCommands();

            size_t visible = _visibleLines();
            for (size_t i = 0; i < visible && _top + i < _lines.size(); ++i)
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    _lines[_top + i],
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, static_cast<uint8_t>(2 + i) }));
            }

            bindInput(InputID::ENC_UP,   _top > 0 ? "^" : "");
            bindInput(InputID::ENC_DOWN, _top + visible < _lines.size() ? "v" : "");
        }
    };

} // namespace DisplayModule
