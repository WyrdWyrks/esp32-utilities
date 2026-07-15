#pragma once

#include "Window.hpp"
#include "States/SavedLocationsState.hpp"
#include "States/EditStringState.hpp"
#include "DisplayUtilities.hpp"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // EditSavedLocationsWindow
    // -------------------------------------------------------------------------
    // Manage saved GPS locations: scroll, rename, and delete. Tracking is now
    // live inside SavedLocationsState — the RingPoint LED points at whichever
    // location is selected as you scroll, so there is no separate "Track" state.
    //
    // State flow:
    //   SavedLocationsState (list view + live tracking)
    //     ↓ BUTTON_4 ("Edit")  — push EditStringState to rename selected location
    //     ↓ BUTTON_1 ("Del")   — delete selected location (handled in state)
    //     ↓ BUTTON_3 ("Back")  — pop window
    //
    //   EditStringState (rename)
    //     ↓ BUTTON_2 ("Done")  — pop state with result → SavedLocationsState
    //     ↓ BUTTON_3 ("Back")  — pop state without result
    //
    // Usage:
    //   Utilities::pushWindow(std::make_shared<EditSavedLocationsWindow>());

    class EditSavedLocationsWindow : public Window
    {
    public:
        EditSavedLocationsWindow()
        {
            _listState     = std::make_shared<SavedLocationsState>();
            _editStrState  = std::make_shared<EditStringState>();

            // ----------------------------------------------------------------
            // BUTTON_3 — context-sensitive Back
            // ----------------------------------------------------------------
            registerInput(InputID::BUTTON_3, "Back");
            addInputCommand(InputID::BUTTON_3,
                [this](const InputContext &ctx)
                {
                    if (_currentState == _listState)
                    {
                        Utilities::popWindow();
                    }
                    else if (_currentState == _editStrState)
                    {
                        // Cancel rename → return to list (no result payload)
                        popState();
                    }
                });

            // ----------------------------------------------------------------
            // BUTTON_4 — Edit selected location name
            // ----------------------------------------------------------------
            registerInput(InputID::BUTTON_4, "Edit");
            addInputCommand(InputID::BUTTON_4,
                [this](const InputContext &ctx)
                {
                    if (_currentState != _listState) return;
                    if (!_listState->hasLocations()) return;

                    StateTransferData d;
                    d.inputID = ctx.inputID;
                    d.payload = _listState->buildEditPayload();
                    pushState(_editStrState, d);
                });

            // ----------------------------------------------------------------
            // BUTTON_1 — Delete (handled directly in SavedLocationsState::handleInput)
            // ----------------------------------------------------------------
            registerInput(InputID::BUTTON_1, "Del");

            // ----------------------------------------------------------------
            // EditStringState BUTTON_2 confirm — pop with result
            // ----------------------------------------------------------------
            addInputCommand(InputID::BUTTON_2,
                [this](const InputContext &ctx)
                {
                    if (_currentState != _editStrState) return;
                    StateTransferData d;
                    d.inputID = ctx.inputID;
                    d.payload = _editStrState->buildResultPayload();
                    popState(d); // → SavedLocationsState::onEnter sees "return" key
                });

            setInitialState(_listState);
        }

    private:
        std::shared_ptr<SavedLocationsState> _listState;
        std::shared_ptr<EditStringState>     _editStrState;
    };

} // namespace DisplayModule
