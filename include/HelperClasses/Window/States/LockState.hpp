#pragma once

#include <vector>
#include <functional>
#include <cstdint>
#include "WindowState.hpp"
#include "DisplayUtilities.hpp"
#include "TextDrawCommand.hpp"
#include "LedUtilities.hpp"
#include "IlluminateButton.hpp"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // LockState
    // -------------------------------------------------------------------------
    // Prompts the user to enter a specific sequence of button presses to
    // "unlock" — i.e. advance to the next window state.  Commonly used as a
    // confirmation gate before destructive or privileged operations.
    //
    // Default sequence: BUTTON_3 then BUTTON_4  (same as the old Lock_State).
    // Override via initializeInputSequence().
    //
    // Lifecycle hooks for LED feedback:
    //   onSequenceReset()   — called when the sequence starts / is reset
    //   onSequenceAdvance() — called after a correct press (currentStep updated)
    //   onSequenceComplete()— called when the last press is correct; the state
    //                         then calls win.popState() or win.switchState().
    //   onWrongInput()      — called when an incorrect button is pressed
    //
    // These do nothing by default.  Subclass LockState and override them to
    // drive LED patterns, sounds, etc.
    //
    // _allowInterrupts is set to false so the lock screen cannot be dismissed
    // by an unexpected window push.
    //
    // Wiring example in a Window:
    //   auto lockState = std::make_shared<LockState>();
    //   win.addInputCommand(InputID::BUTTON_1, [lockState](auto &ctx) {
    //       lockState->processButton(win, ctx.inputID);
    //   });
    //   // repeat for BUTTON_2, BUTTON_3, BUTTON_4 …
    //
    // Or more concisely, wire all four buttons in a loop:
    //   for (uint8_t id : {InputID::BUTTON_1, InputID::BUTTON_2,
    //                       InputID::BUTTON_3, InputID::BUTTON_4})
    //   {
    //       win.addInputCommand(id, [lockState, &win, id](auto &) {
    //           lockState->processButton(win, id);
    //       });
    //   }

    class LockState : public WindowState
    {
    public:
        LockState()
        {
            _allowInterrupts = false;
            _sequence        = { InputID::BUTTON_3, InputID::BUTTON_4 };
            _step            = 0;
        }

        // ------------------------------------------------------------------
        // Sequence configuration
        // ------------------------------------------------------------------

        void initializeInputSequence(std::vector<uint8_t> sequence)
        {
            _sequence = std::move(sequence);
            _step     = 0;
        }

        size_t  sequenceLength()  const { return _sequence.size(); }
        size_t  currentStep()     const { return _step; }
        uint8_t expectedInput()   const
        {
            return (_step < _sequence.size()) ? _sequence[_step] : 0xFF;
        }

        // ------------------------------------------------------------------
        // Lifecycle
        // ------------------------------------------------------------------

        void onEnter(const StateTransferData &) override
        {
            _step = 0;
            _rebuildDrawCommands();
            onSequenceReset();
            // IlluminateButton is registered at MODAL priority, so while it is
            // enabled it owns the button LEDs and press feedback can't touch them.
            _illuminateID = UxModule::IlluminateButton::RegisteredPatternID();
            UxModule::LedUtilities::enablePattern(_illuminateID);
            _configureLed();
        }

        void onExit() override
        {
            UxModule::LedUtilities::disablePattern(_illuminateID);
            WindowState::onExit();
        }

        // ------------------------------------------------------------------
        // Input processing
        // ------------------------------------------------------------------

        // Call this from the Window's onInputCommand for each relevant button.
        void processButton(const InputContext &ctx)
        {
            if (_sequence.empty()) return;

            if (ctx.inputID == _sequence[_step])
            {
                ++_step;
                if (_step >= _sequence.size())
                {
                    // Sequence complete — fire the completion action
                    if (_completionFn)
                    {
                        _completionFn();
                    }
                }
                else
                {
                    onSequenceAdvance();
                    _rebuildDrawCommands();
                }
            }
            else
            {
                _step = 0;
                _rebuildDrawCommands();
                onWrongInput();
                onSequenceReset();
            }

            _configureLed();
        }

        // handleInput is intentionally left as a no-op here.
        // Callers must wire buttons via Window onInputCommands and call
        // processButton() themselves so they can target this state directly
        // while sharing the Window's input map.

        // ------------------------------------------------------------------
        // Completion action
        // ------------------------------------------------------------------
        // By default, completing the sequence calls win.popState().
        // Set a custom completion callback to override this — e.g. to
        // switchState() to a specific next state instead of popping.
        //
        //   lockState->setOnSequenceComplete([&win, nextState]() {
        //       win.switchState(nextState, buildPayload());
        //   });

        using CompletionFn = std::function<void()>;

        void setOnSequenceComplete(CompletionFn fn)
        {
            _completionFn = std::move(fn);
        }

        // ------------------------------------------------------------------
        // LED / audio hook overrides — override in subclasses
        // ------------------------------------------------------------------

        virtual void onSequenceReset()   {}
        virtual void onSequenceAdvance() {}
        virtual void onWrongInput()      { _step = 0; }

    private:
        std::vector<uint8_t> _sequence;
        size_t               _step = 0;
        CompletionFn         _completionFn;
        int                  _illuminateID = -1;

        void _rebuildDrawCommands()
        {
            clearDrawCommands();
        }

        void _configureLed()
        {
            JsonDocument cfg;
            auto array = cfg["inputStates"].to<ArduinoJson::JsonArray>();

            for (auto inputID : _sequence)
            {
                auto inputState = array.add<ArduinoJson::JsonObject>();

                inputState["input"] = inputID;

                if (inputID == _sequence[_step])
                {
                    inputState["state"] = true;
                }
                else
                {
                    inputState["state"] = false;
                }
            }            

            UxModule::LedUtilities::configurePattern(_illuminateID, cfg);
            UxModule::LedUtilities::iteratePattern(_illuminateID);
        }
    };

} // namespace DisplayModule
