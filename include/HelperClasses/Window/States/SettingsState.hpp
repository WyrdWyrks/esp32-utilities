#pragma once

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstddef>
#include "WindowState.hpp"
#include "DisplayUtilities.hpp"
#include "TextDrawCommand.hpp"
#include <ArduinoJson.h>
#include "FilesystemUtils.h"
#include "SettingsInterface.hpp"

// Edit state headers — full definitions required so shared_ptr<EditXxxState>
// is implicitly convertible to shared_ptr<WindowState> in selectCurrent().
#include "EditBoolState.hpp"
#include "EditIntState.hpp"
#include "EditFloatState.hpp"
#include "EditEnumState.hpp"
#include "EditStringState.hpp"

namespace DisplayModule
{
    class SettingsState : public WindowState
    {
    public:
        SettingsState()
        {
            _settingsIt = FilesystemModule::Utilities::DeviceSettings().begin();

            bindInput(InputID::ENC_UP, "", [this](const InputContext &){
                auto &settings = FilesystemModule::Utilities::DeviceSettings();
                if (settings.size() < 2) return;
                _settingsIt = (_settingsIt == settings.begin()) ? std::prev(settings.end()) : std::prev(_settingsIt);
                _rebuildDrawCommands();
            });
            bindInput(InputID::ENC_DOWN, "", [this](const InputContext &){
                auto &settings = FilesystemModule::Utilities::DeviceSettings();
                if (settings.size() < 2) return;
                _settingsIt = (_settingsIt == std::prev(settings.end())) ? settings.begin() : std::next(_settingsIt);
                _rebuildDrawCommands();
            });
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &data) override
        {
            // Returning from an edit state — apply result if present
            if (data.inputID != 0 || data.payload)
                _applyEditResult(data);

            _updateInputBindings();
            _rebuildDrawCommands();
        }

        // ------------------------------------------------------------------
        // buildExitData()
        // ------------------------------------------------------------------
        // Called by the Window before pushing an edit state.  Builds and
        // returns the StateTransferData payload appropriate for the currently
        // selected leaf node so the edit state can initialise itself in
        // onEnter().  Returns an empty payload for GROUP / ACTION nodes
        // (those are handled internally by activateSelection()).
        // ------------------------------------------------------------------
        StateTransferData buildExitData(uint8_t inputID) override
        {
            StateTransferData d;
            d.payload = std::make_shared<JsonDocument>();
            if (!(*_settingsIt))
            {
                return d;
            }
            d.inputID = inputID;

            auto settings = FilesystemModule::Utilities::DeviceSettings();
            if (settings.empty()) return d;

            const auto &node = *_settingsIt;
            auto valueObj = (*d.payload).to<JsonObject>();
            node->toJson(valueObj);
            ESP_LOGI(TAG, "Built payload for selection with type '%s'", node->getType().c_str());

            if (d.payload)
            {
                auto jsonStr = d.payload->as<std::string>();
                ESP_LOGI(TAG, "Built payload for selection: %s", jsonStr.c_str());
            }
            else
            {
                ESP_LOGI(TAG, "No payload for selection");
            }

            return d;
        }

        std::string getSelectionType() const
        {
            auto settings = FilesystemModule::Utilities::DeviceSettings();
            if (settings.empty()) return "";
            return (*_settingsIt)->getType();
        }

    private:

        FilesystemModule::SettingsMap::iterator _settingsIt;

        // ------------------------------------------------------------------
        // Input binding — called after every navigation step so WindowLayer
        // always shows labels that match the current context.
        // ------------------------------------------------------------------

        void _updateInputBindings()
        {
            bindInput(InputID::BUTTON_3, "Back");
            bindInput(InputID::BUTTON_4, "Edit");

            // Scroll arrows — active whenever the current level has multiple items.
            // Lambdas call _updateInputBindings() again after changing _index so
            // BUTTON_3/4 labels stay in sync with the newly selected item.
            // EventHandler snapshots before dispatch so re-entrant replacement is safe.
            if (FilesystemModule::Utilities::DeviceSettings().size() > 1)
            {
                bindInput(InputID::ENC_UP, "^");
                bindInput(InputID::ENC_DOWN, "v");
            }
            else
            {
                bindInput(InputID::ENC_UP, "");
                bindInput(InputID::ENC_DOWN, "");
            }
        }

        // ------------------------------------------------------------------
        // Apply result returned from an edit state
        // ------------------------------------------------------------------

        void _applyEditResult(const StateTransferData &data)
        {
            if (!data.payload) 
            {
                ESP_LOGW(TAG, "No payload returned from edit state; nothing to apply");
                return;
            }
            if ((*data.payload)[FilesystemModule::SettingsInterface::write_key].isNull())
            {
                ESP_LOGW(TAG, "Edit state payload missing expected key '%s'; cannot apply result", FilesystemModule::SettingsInterface::write_key);
                return;
            }

            auto payloadObj = data.payload->as<JsonObjectConst>();
            std::string debugStr;
            serializeJson(payloadObj, debugStr);
            ESP_LOGI(TAG, "Applying edit result with payload: %s", debugStr.c_str());
            (*_settingsIt)->fromJson(payloadObj);
            (*_settingsIt)->saveToPreferences(FilesystemModule::Utilities::SettingsPreference());
        }

        // ------------------------------------------------------------------
        // Draw commands
        // ------------------------------------------------------------------

        void _rebuildDrawCommands()
        {
            clearDrawCommands();

            if (FilesystemModule::Utilities::DeviceSettings().empty()) return;

            // Selected item label — centred
            addDrawCommand(std::make_shared<TextDrawCommand>(
                (*_settingsIt)->key,
                TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 3 }
            ));

            // Current value hint for leaf nodes
            const auto &node = *_settingsIt;
            std::string hint = _valueHint(node);
            if (!hint.empty())
            {
                addDrawCommand(std::make_shared<TextDrawCommand>(
                    hint,
                    TextFormat{ TextAlignH::CENTER, TextAlignV::LINE, 4 }
                ));
            }

        }

        static std::string _valueHint(const std::shared_ptr<FilesystemModule::SettingsInterface> &node)
        {
            auto type = node->getType();
            JsonDocument doc;
            auto valueObj = doc.to<JsonObject>();
            node->toJson(valueObj);

            std::string dbgHint;
            serializeJson(valueObj, dbgHint);
            ESP_LOGI(TAG, "Generating value hint for type '%s' with JSON: %s", type.c_str(), dbgHint.c_str());

            if (type == FilesystemModule::BoolSetting::type)
            {
                bool val = valueObj[FilesystemModule::SettingsInterface::value_key].as<bool>();
                return val ? "[ON]" : "[OFF]";
            }
            else if (type == FilesystemModule::EnumSetting::type)
            {
                int idx = valueObj[FilesystemModule::SettingsInterface::value_key].as<int>();
                auto arr = valueObj[FilesystemModule::EnumSetting::labels_key].as<JsonArray>();
                if (idx >= 0 && idx < arr.size())
                    return arr[idx].as<std::string>();
                else
                    return "Invalid";
            }
            else
            {
                std::string val = valueObj[FilesystemModule::SettingsInterface::value_key].as<std::string>();
                return val;
            }
        }
    };
} // namespace DisplayModule
