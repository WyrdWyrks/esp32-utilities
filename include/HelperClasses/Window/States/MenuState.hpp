#pragma once

#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include "WindowState.hpp"
#include "DisplayUtilities.hpp"
#include "TextDrawCommand.hpp"
#include "ScrollWheel.hpp"

namespace DisplayModule
{
    // Forward declaration — Window is in DisplayUtilities.hpp forward decls
    class Window;

    // -------------------------------------------------------------------------
    // MenuItem
    // -------------------------------------------------------------------------
    // A single entry in a MenuState list.
    //
    // label         — display text shown in the centre of the screen.
    // onSelect      — callback fired when selected.
    //
    // Typically only one of onSelect / adjacentState is used per item, but
    // both may be set (onSelect fires first).

    struct MenuItem
    {
        std::string                    label;
        std::function<void()>  onSelect;

        MenuItem() = default;

        explicit MenuItem(std::string lbl,
                          std::function<void()> cb = nullptr)
            : label(std::move(lbl))
            , onSelect(std::move(cb))
        {}
    };

    // -------------------------------------------------------------------------
    // MenuState
    // -------------------------------------------------------------------------
    // Generic scrollable single-item menu state.
    //
    // The visible item is always centred on screen.  Scroll arrows are shown
    // (via label hints) when there are multiple items.
    //
    // Input handling (registered once in the constructor):
    //   ENC_UP   → scrollUp()   (no-op when fewer than 2 items)
    //   ENC_DOWN → scrollDown() (no-op when fewer than 2 items)
    //   BUTTON_4 → label "Select" (action wired by the owning Window)
    //
    // Selection:
    //   Call selectCurrent() from the Window's BUTTON_4 onInputCommand.
    //   This fires item.onSelect (if set).
    //
    // Draw commands are rebuilt on enter and after each scroll so ContentLayer
    // always reflects the current position without any per-frame logic.

    class MenuState : public WindowState
    {
    public:
        MenuState()
        {
            bindInput(InputID::BUTTON_4, "Select");
            bindInput(InputID::ENC_UP, "^", [this](const InputContext &) {
                scrollUp();
            });
            bindInput(InputID::ENC_DOWN, "v", [this](const InputContext &) {
                scrollDown();
            });
        }

        // ------------------------------------------------------------------
        // Item management
        // ------------------------------------------------------------------

        void addItem(MenuItem item)
        {
            ESP_LOGV(TAG, "Adding menu item: %s", item.label.c_str());
            _items.push_back(std::move(item));
            _updateScrollLabels();
            _configureLed();
            _rebuildDrawCommands();
        }

        void addItem(std::string label,
                     std::function<void()> cb = nullptr)
        {
            addItem(MenuItem(std::move(label), std::move(cb)));
        }

        void clearItems()
        {
            _items.clear();
            _index = 0;
        }

        size_t itemCount()    const { return _items.size(); }
        size_t currentIndex() const { return _index; }

        const MenuItem *currentItem() const
        {
            return _items.empty() ? nullptr : &_items[_index];
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &) override
        {
            _index = 0;
            _updateScrollLabels();

            _scrollWheelPatternId = UxModule::ScrollWheel::RegisteredPatternID();
            UxModule::LedUtilities::enablePattern(_scrollWheelPatternId);
            
            _configureLed();
            _rebuildDrawCommands();
        }

        void onExit() override
        {
            if (_scrollWheelPatternId >= 0)
            {
                UxModule::LedUtilities::clearPattern(_scrollWheelPatternId);
                UxModule::LedUtilities::disablePattern(_scrollWheelPatternId);
            }
            WindowState::onExit();
        }

        // ------------------------------------------------------------------
        // Selection — call from the Window's BUTTON_4 onInputCommand.
        // ------------------------------------------------------------------

        void selectCurrent()
        {
            if (_items.empty()) return;
            auto &item = _items[_index];

            if (item.onSelect)
            {
                item.onSelect();
                ESP_LOGV(TAG, "Selected menu item: %s", item.label.c_str());
            }
            else
            {
                ESP_LOGV(TAG, "Selected menu item with no action: %s", item.label.c_str());
            }
                
        }

        // ------------------------------------------------------------------
        // Scroll helpers (also callable from Window command lambdas)
        // ------------------------------------------------------------------

        void scrollUp()
        {
            if (_items.empty()) {
                ESP_LOGW(TAG, "Attempted to scroll up in an empty menu");
                return;
            }
            _index = (_index == 0) ? _items.size() - 1 : _index - 1;
            ESP_LOGV(TAG, "Scrolled up to index %d", _index);
            _configureLed();
            _rebuildDrawCommands();
        }

        void scrollDown()
        {
            if (_items.empty()) {
                ESP_LOGW(TAG, "Attempted to scroll down in an empty menu");
                return;
            }
            _index = (_index + 1) % _items.size();
            ESP_LOGV(TAG, "Scrolled down to index %d", _index);
            _configureLed();
            _rebuildDrawCommands();
        }

        void onPause() override
        {
            if (_scrollWheelPatternId >= 0)
            {
                UxModule::LedUtilities::clearPattern(_scrollWheelPatternId);
                UxModule::LedUtilities::disablePattern(_scrollWheelPatternId);
            }
        }

        void onResume() override
        {
            if (_scrollWheelPatternId >= 0)
            {
                UxModule::LedUtilities::enablePattern(_scrollWheelPatternId);
                _configureLed();
            }
        }

    private:
        std::vector<MenuItem> _items;
        size_t                _index = 0;
        int                   _scrollWheelPatternId = -1;

        // Update the ENC_UP/ENC_DOWN label hints to show arrows only when
        // there are multiple items to scroll through.  Actions are not
        // touched — they are registered once in the constructor.
        void _updateScrollLabels()
        {
            if (_items.size() > 1)
            {
                bindInput(InputID::ENC_UP,   "^");
                bindInput(InputID::ENC_DOWN, "v");
            }
            else
            {
                bindInput(InputID::ENC_UP,   "");
                bindInput(InputID::ENC_DOWN, "");
            }
        }

        void _configureLed()
        {
            if (_scrollWheelPatternId < 0) return;

            
            if (!_items.empty())
            {
                JsonDocument cfg;
                cfg["numItems"] = _items.size();
                cfg["currItem"] = _index; 

                UxModule::LedUtilities::configurePattern(_scrollWheelPatternId, cfg);
                UxModule::LedUtilities::iteratePattern(_scrollWheelPatternId);
            }
        }

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            if (_items.empty())
            {
                ESP_LOGW(TAG, "No items in menu to display");
                return;
            }

            ESP_LOGV(TAG, "Rebuilding draw commands for menu with %d items (current index: %d)", _items.size(), _index);

            const auto &label = _items[_index].label;

            // Current item — centred
            addDrawCommand(std::make_shared<TextDrawCommand>(
                label,
                TextFormat{ TextAlignH::CENTER, TextAlignV::CENTER }
            ));
        }
    };

} // namespace DisplayModule
