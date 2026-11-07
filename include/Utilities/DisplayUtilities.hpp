#pragma once

#include <stack>
#include <map>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstring>
#include "SystemUtilities.hpp"
#include "Interfaces/DisplayTypes.hpp"
#include "Interfaces/WindowInterface.hpp"
#include "Interfaces/LayerInterface.hpp"
#include "Interfaces/EventHandler.hpp"

// Define the pure black color.
#if HARDWARE_VERSION >= 3
    #define BLACK 0
#else
    #define BLACK 0
#endif

namespace DisplayModule
{  
    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------
    // Single point of contact for the display module:
    //   - Window stack (owns all window lifetimes via shared_ptr<WindowInterface>)
    //   - Display reference + DrawContext
    //   - FreeRTOS queue (managed via System_Utils ID)
    //   - Layer pipeline (map<uint8_t, shared_ptr<LayerInterface>>)
    //   - Refresh interval driven per active window/state
    //   - Text layout helpers (all inline — no .cpp needed)
    //   - Legacy callback map (bridge during migration)

    class Utilities
    {
    public:
        // ------------------------------------------------------------------
        // Events
        // ------------------------------------------------------------------

        // Fired at the end of every render pass, after all layers have been
        // drawn and the window has been notified.
        //
        // Wire this up in the application to commit the framebuffer to the
        // physical display.  For Adafruit_SSD1306 that means calling
        // display.display().
        //
        // Example (Wayfinder project setup):
        //   DisplayModule::Utilities::onRenderComplete += [&]()
        //   {
        //       ssd1306Display.display();
        //   };
        inline static EventHandler<> onRenderComplete;

        // ------------------------------------------------------------------
        // Initialisation — call once from Manager::init()
        // ------------------------------------------------------------------
        static void init()
        {
            ESP_LOGI(TAG, "Initializing DisplayUtilities");
            static uint8_t       queueStorage[QUEUE_LENGTH * sizeof(DisplayCommandQueueItem)];
            static StaticQueue_t queueBuffer;
            _queueID = System_Utils::registerQueue(
                QUEUE_LENGTH,
                sizeof(DisplayCommandQueueItem),
                queueStorage,
                queueBuffer
            );
        }

        // ------------------------------------------------------------------
        // Display hardware
        // ------------------------------------------------------------------
        static void setDisplay(Adafruit_GFX *disp, uint16_t w, uint16_t h)
        {
            ESP_LOGI(TAG, "Display set with dimensions %dx%d", w, h);
            _drawCtx().display = disp;
            _drawCtx().width   = w;
            _drawCtx().height  = h;
        }

        static DrawContext &drawContext() { return _drawCtx(); }

        // ------------------------------------------------------------------
        // Command queue
        // ------------------------------------------------------------------
        static QueueHandle_t getDisplayCommandQueue()
        {
            return System_Utils::getQueue(_queueID);
        }

        static void sendInputCommand(uint8_t inputID)
        {
            DisplayCommandQueueItem item;
            item.commandType                      = CommandType::INPUT_COMMAND;
            item.commandData.inputCommand.inputID = inputID;
            System_Utils::sendToQueue(_queueID, &item, 0);
        }

        // Queues a registered callback (see registerCallback) to run on the
        // display task. The queue is one slot deep, so a producer on another
        // task that must not lose the request can wait up to timeoutMs for the
        // display task to drain the previous item; the default drops it if the
        // slot is taken. Returns whether the command was queued.
        static bool sendCallbackCommand(uint32_t resourceID, size_t timeoutMs = 0)
        {
            DisplayCommandQueueItem item;
            item.commandType                            = CommandType::CALLBACK_COMMAND;
            item.commandData.callbackCommand.resourceID = resourceID;
            return System_Utils::sendToQueue(_queueID, &item, timeoutMs);
        }

        // Asks the display task for one out-of-band refresh: it ticks the
        // active window (the same onTick() its refresh interval drives) and
        // renders, then goes back to waiting on the queue. Safe to call from
        // any task; this never draws on the caller's task. Same one-slot
        // queue semantics as sendCallbackCommand: timeoutMs bounds how long to
        // wait for a free slot, 0 drops the request if one isn't free.
        static bool sendRefreshCommand(size_t timeoutMs = 0)
        {
            DisplayCommandQueueItem item;
            item.commandType = CommandType::REFRESH_COMMAND;
            return System_Utils::sendToQueue(_queueID, &item, timeoutMs);
        }

        // ------------------------------------------------------------------
        // Window stack
        // ------------------------------------------------------------------
        static void pushWindow(std::shared_ptr<WindowInterface> window)
        {
            if (!_windowStack().empty())
                _windowStack().top()->onPause();

            _windowStack().push(std::move(window));
            _windowStack().top()->onResume();

            render();
        }

        static void popWindow(const InputContext &_ = InputContext{})
        {
             if (_windowStack().empty()) return;

            _windowStack().top()->onPause();

            if (_windowStack().top()->currentState())
            {
                _windowStack().top()->currentState()->onExit();
            }

            _windowStack().pop();

            if (!_windowStack().empty())
                _windowStack().top()->onResume();

            render();
        }

        static std::shared_ptr<WindowInterface> activeWindow()
        {
            if (_windowStack().empty()) return nullptr;
            return _windowStack().top();
        }

        static bool hasWindow() { return !_windowStack().empty(); }

        static bool isWindowStackAtRoot() { return _windowStack().size() == 1; }

        // ------------------------------------------------------------------
        // Input dispatch
        // ------------------------------------------------------------------
        static void handleInput(const InputContext &ctx)
        {
            getInputRaised()(ctx);
            vTaskDelay(1); 

            auto win = activeWindow();
            if (win) 
            {
                win->handleInput(ctx);
            }
        }

        // ------------------------------------------------------------------
        // Legacy callback dispatch — bridge during migration
        // ------------------------------------------------------------------
        static void handleCallback(uint32_t resourceID)
        {
            auto &map = _callbackMap();
            auto it = map.find(resourceID);
            if (it != map.end())
                it->second(resourceID);
        }

        static void registerCallback(uint32_t resourceID,
                                     std::function<void(uint32_t)> cb)
        {
            _callbackMap()[resourceID] = std::move(cb);
        }

        // ------------------------------------------------------------------
        // Layer pipeline
        // ------------------------------------------------------------------
        static void registerLayer(uint8_t layerID,
                                  std::shared_ptr<LayerInterface> layer)
        {
            _layers()[layerID] = std::move(layer);
        }

        static void removeLayer(uint8_t layerID)
        {
            _layers().erase(layerID);
        }

        static std::shared_ptr<LayerInterface> getLayer(uint8_t layerID)
        {
            auto &m = _layers();
            auto it = m.find(layerID);
            return (it != m.end()) ? it->second : nullptr;
        }

        // ------------------------------------------------------------------
        // Render — iterates layers ascending, commits the framebuffer via
        // onRenderComplete, then notifies the window via handleRendered.
        // ------------------------------------------------------------------
        static void render()
        {
            _drawCtx().display->fillScreen(0); // Clear framebuffer before drawing layers

            auto win = activeWindow();
            if (!win) 
            {
                ESP_LOGW(TAG, "Attempting to render with no active window");
                return;
            }

            ESP_LOGV(TAG, "Rendering frame for %d layers",
                     static_cast<int>(_layers().size()));

            for (auto &[id, layer] : _layers())
            {
                if (layer && layer->isEnabled())
                    layer->draw(_drawCtx());
            }

            // Commit framebuffer to the physical display.
            // Subscribers (e.g. Adafruit_SSD1306::display()) are called here.
            onRenderComplete();

            win->handleRendered();
        }

        // ------------------------------------------------------------------
        // Queue timeout — drives autonomous redraw via the task loop.
        // Returns the active window's refresh interval in ticks, or
        // portMAX_DELAY if no interval is set (redraw on input only).
        // ------------------------------------------------------------------
        static TickType_t queueTimeout()
        {
            auto win = activeWindow();
            if (!win) return portMAX_DELAY;

            uint32_t ms = win->activeStateRefreshInterval();
            return (ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
        }

        // ------------------------------------------------------------------
        // Text layout helpers (all inline)
        // ------------------------------------------------------------------
        static uint16_t centerTextHorizontal(size_t charCount, int distanceFrom = 0)
        {
            constexpr uint16_t CHAR_W = 6;
            return static_cast<uint16_t>(
                (_drawCtx().width / 2)
                - ((charCount * CHAR_W) / 2)
                + (distanceFrom * CHAR_W)
            );
        }

        static uint16_t centerTextHorizontal(const char *text, int distanceFrom = 0)
        {
            return centerTextHorizontal(strlen(text), distanceFrom);
        }

        static uint16_t centerTextVertical()
        {
            constexpr uint16_t CHAR_H = 8;
            return static_cast<uint16_t>((_drawCtx().height / 2) - (CHAR_H / 2));
        }

        static uint16_t selectTextLine(uint8_t line)
        {
            constexpr uint16_t CHAR_H = 8;
            return static_cast<uint16_t>((line - 1) * CHAR_H);
        }

        static uint16_t alignTextLeft(int distanceFrom = 0)
        {
            return static_cast<uint16_t>(distanceFrom * 6);
        }

        static uint16_t alignTextRight(size_t charCount, int distanceFrom = 0)
        {
            constexpr uint16_t CHAR_W = 6;
            return static_cast<uint16_t>(
                _drawCtx().width
                - (charCount * CHAR_W)
                - (distanceFrom * CHAR_W)
            );
        }

        static uint16_t alignTextRight(const char *text, int distanceFrom = 0)
        {
            return alignTextRight(strlen(text), distanceFrom);
        }

        static size_t getIntLength(int64_t num)
        {
            if (num == 0) return 1;
            size_t  len = (num < 0) ? 1 : 0;
            int64_t n   = (num < 0) ? -num : num;
            while (n > 0) { ++len; n /= 10; }
            return len;
        }

        static size_t getUintLength(uint64_t num)
        {
            if (num == 0) return 1;
            size_t len = 0;
            while (num > 0) { ++len; num /= 10; }
            return len;
        }

        // Returns the index (1-based) of the last full text line that fits
        // within the display height.  Used for bottom-anchored indicators.
        static uint8_t selectBottomTextLine()
        {
            constexpr uint16_t CHAR_H = 8;
            return static_cast<uint8_t>(_drawCtx().height / CHAR_H);
        }

        static void clearAndDisplay(std::shared_ptr<DrawCommand> cmd, size_t displayTimeMs = 2000)
        {
            auto &drawCtx = Utilities::drawContext();
            drawCtx.display->fillScreen(0);
            cmd->draw(drawCtx);
            Utilities::onRenderComplete();
            vTaskDelay(pdMS_TO_TICKS(displayTimeMs));
        }

        static EventHandler<const InputContext &> &getInputRaised() 
        { 
            static EventHandler<const InputContext &> handler;
            return handler;
        }

    private:
        Utilities() = delete;

        static constexpr size_t QUEUE_LENGTH = 1;

        static inline int _queueID = -1;

        static DrawContext &_drawCtx()
        {
            static DrawContext ctx;
            return ctx;
        }

        static std::stack<std::shared_ptr<WindowInterface>> &_windowStack()
        {
            static std::stack<std::shared_ptr<WindowInterface>> stack;
            return stack;
        }

        static std::unordered_map<uint32_t, std::function<void(uint32_t)>> &_callbackMap()
        {
            static std::unordered_map<uint32_t, std::function<void(uint32_t)>> map;
            return map;
        }

        static std::map<uint8_t, std::shared_ptr<LayerInterface>> &_layers()
        {
            static std::map<uint8_t, std::shared_ptr<LayerInterface>> layers;
            return layers;
        }
    };

} // namespace DisplayModule
