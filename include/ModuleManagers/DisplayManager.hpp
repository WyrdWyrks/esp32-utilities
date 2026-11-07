#pragma once

#include <Adafruit_GFX.h>
#include "DisplayUtilities.hpp"
#include "SystemUtilities.hpp"
#include "LED_Manager.h"

namespace DisplayModule
{
    // -------------------------------------------------------------------------
    // Manager
    // -------------------------------------------------------------------------
    // Pure task/dispatch loop for the display module.
    // Holds no window knowledge — all window logic lives in Utilities + Window.
    //
    // Typical application startup sequence:
    //   // 1. Initialise your hardware display (application code)
    //   display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    //   display.clearDisplay();
    //   display.setTextSize(1);
    //   display.setTextColor(SSD1306_WHITE);
    //
    //   // 2. Hand the display to the module
    //   DisplayModule::Manager::init(&display, 128, 32);
    //
    //   // 3. Register layers
    //   DisplayModule::Utilities::registerLayer(
    //       DisplayModule::LayerID::CONTENT,
    //       std::make_shared<DisplayModule::ContentLayer>());
    //   DisplayModule::Utilities::registerLayer(
    //       DisplayModule::LayerID::WINDOW,
    //       std::make_shared<DisplayModule::WindowLayer>());
    //
    //   // 4. Push your root window
    //   DisplayModule::Utilities::pushWindow(std::make_shared<MyRootWindow>());
    //
    //   // 5. Start the task
    //   DisplayModule::Manager::startTask();

    class Manager
    {
    public:
        // Call once at startup.
        // disp     — pointer to an already-initialised Adafruit_GFX display.
        // width/height — display dimensions in pixels.
        void Initialize(Adafruit_GFX *disp, uint16_t width, uint16_t height)
        {
            Utilities::init();
            Utilities::setDisplay(disp, width, height);
            startTask();
        }

        // Register the display command queue processing task via System_Utils.
        // Call after init().
        void startTask(UBaseType_t priority = 2, BaseType_t coreID = 0)
        {
            ESP_LOGI(TAG, "Starting DisplayManager task on core %d with priority %d", coreID, priority);
            System_Utils::registerTask(
                [](void * instancePtr) 
                { 
                    static_cast<Manager*>(instancePtr)->processCommandQueue(); 
                }, 
                "DisplayManager", 
                TASK_STACK_SIZE, 
                (void *)this, 
                priority,
                coreID);
        }

    private:
        static constexpr uint32_t TASK_STACK_SIZE = 4096;

        // FreeRTOS task entry point.
        // Reads the display command queue in a loop and dispatches to Utilities.
        //
        // Queue timeout doubles as the autonomous refresh heartbeat:
        //   - Active state returns a non-zero interval → timeout = that interval.
        //     Render fires and Window::onTick() is called (e.g. cursor blink).
        //   - Active state returns 0 → timeout = portMAX_DELAY.
        //     Redraw only happens when input arrives.
        //
        // Render is called after every loop iteration regardless of whether
        // an item was received or the timeout fired.
        void processCommandQueue()
        {
            ESP_LOGI(TAG, "DisplayManager task started, entering command processing loop");
            DisplayCommandQueueItem item;
            TickType_t lastButtonPressTick = 0;

            for (;;)
            {
                TickType_t timeout  = Utilities::queueTimeout();
                bool       gotItem  = xQueueReceive(
                                          Utilities::getDisplayCommandQueue(),
                                          &item,
                                          timeout) == pdTRUE;
                bool       timedOut = !gotItem;
                bool       refreshRequested = false;

                if (gotItem)
                {
                    switch (item.commandType)
                    {
                        case CommandType::INPUT_COMMAND:
                        {
                            ESP_LOGI(TAG, "Received input command with ID %d", item.commandData.inputCommand.inputID);
                            // Debounce — discard inputs faster than DEBOUNCE_DELAY_MS
                            TickType_t now = xTaskGetTickCount();
                            if ((now - lastButtonPressTick) * portTICK_PERIOD_MS
                                    < DEBOUNCE_DELAY_MS)
                                break;
                            lastButtonPressTick = now;

                            // haptic feedback
                            LED_Manager::applyHapticFeedback(150);

                            // Drain duplicates that arrived during the debounce window
                            xQueueReset(Utilities::getDisplayCommandQueue());

                            InputContext ctx;
                            ctx.inputID = item.commandData.inputCommand.inputID;
                            Utilities::handleInput(ctx);

                            break;
                        }

                        case CommandType::CALLBACK_COMMAND:
                            Utilities::handleCallback(
                                item.commandData.callbackCommand.resourceID);
                            break;

                        // An explicit refresh behaves like the refresh
                        // interval firing early: tick the window so it
                        // rebuilds its draw commands, then fall through to the
                        // render below.
                        case CommandType::REFRESH_COMMAND:
                            refreshRequested = true;
                            break;

                        default:
                            break;
                    }
                }

                // Tick the active window only on autonomous refresh (timeout
                // or an explicit REFRESH_COMMAND), not on every input event, so
                // per-frame logic (cursor blink, countdown timers, etc.) runs
                // at the state's requested interval.
                if (timedOut || refreshRequested)
                {
                    auto win = Utilities::activeWindow();
                    if (win) 
                    {
                        ESP_LOGD(TAG, "Autonomous refresh triggered, calling onTick for active window");
                        win->onTick();
                    }
                }

                // Render every iteration
                Utilities::render();
            }
        }
    };

} // namespace DisplayModule
