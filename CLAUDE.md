# CLAUDE.md - esp32-utilities

A hardware-focused ESP32 library providing high-level managers, UI primitives, LED control, LoRa mesh networking, GPS navigation, and more. Designed to be consumed by an application layer project.

## Build System

- **Framework:** PlatformIO with Arduino framework
- **Target:** ESP32 (espressif32, esp32dev board)
- **Build:** `pio run`
- **Clean build:** `pio run --target clean`
- **Library dependency mode:** `deep` (resolves transitive dependencies)

Include paths are explicitly set in `platformio.ini` build_flags — all `include/` subdirectories are on the path.

## Project Structure

```
esp32-utilities/
├── include/                    # Public headers (mirrors src/)
│   ├── Interfaces/             # Abstract interfaces (LED_Pattern_Interface, DrawCommandInterface, etc.)
│   ├── ModuleManagers/         # Top-level system managers
│   ├── HelperClasses/
│   │   ├── DrawCommands/       # UI drawing primitives
│   │   ├── LED_Patterns/       # WS2812B animation patterns
│   │   ├── Message_Types/      # LoRa message type definitions
│   │   ├── Network/            # Network communication classes
│   │   ├── OLED_Content/       # Screen content renderers
│   │   ├── Window/             # Window management classes
|   |   |   └── Window_States/      # State machine definitions
│   │   └── Rpc/                # Remote procedure call framework
│   │   
│   └── Utilities/              # Utility headers
└── src/                        # Implementations (mirrors include/). Not preferred. Stick to hpp files for new code
```

## Architecture

### Layered Design

```
Application Project
    ↓
ModuleManagers (Display, LED, LoRa, GPS, Settings, Filesystem, Rpc)
    ↓
HelperClasses (Windows, States, Content, Patterns, Messages)
    ↓
Utilities + Interfaces
```

### Module Managers (Singletons)

| Manager | Responsibility |
|---|---|
| `Display_Manager` | OLED orchestration, window/state stack, input routing via FreeRTOS queue |
| `LED_Manager` | WS2812B animation, compass ring rendering, pattern registration |
| `Settings_Manager` | JSON settings persistence (SPIFFS) |
| `LoraManager` | LoRa mesh networking, MessagePack serialization |
| `NavigationModule::Manager` | GPS + compass integration, heading/distance calculation |
| `FilesystemManager` | SPIFFS file I/O |
| `EspNowManager` | ESP-NOW protocol |
| `RpcManager` | Remote procedure call infrastructure |

### Key Patterns

- **Singleton:** All managers use static class members (FreeRTOS-safe)
- **State Machine:** `Window_State` + `OLED_Window` implement a state stack for display navigation
- **Queue-Driven:** Display commands flow through FreeRTOS queues; inputs mapped to callbacks via `std::map<uint32_t, callbackPointer>`
- **Interface/Plugin:** `LED_Pattern_Interface` and `DrawCommandInterface` allow registering new behaviors without modifying managers
- **Factory:** `MessageBase::MessageFactory` creates polymorphic LoRa messages


## Coding Conventions

- **Classes:** PascalCase with underscores for multi-word (`LED_Manager`, `OLED_Window`, `Window_State`)
- **Constants/Defines:** `UPPER_CASE` (`DEBOUNCE_DELAY`, `NUM_COMPASS_LEDS`, `BUTTON_1_PIN`)
- **Member variables:** camelCase; static members for class-level state
- **Logging:** `ESP_LOG` macros (ESP-IDF style) — not `Serial.print`
- **C++ standard:** C++17 — use `std::unordered_map`, `std::vector`, `std::string`, lambdas, move semantics
- **JSON:** `ArduinoJson` v7 (`JsonDocument` — elastic capacity)
- **Serialization:** MessagePack via ArduinoJson for LoRa messages

## Key Dependencies

| Library | Purpose |
|---|---|
| FastLED 3.6.0 | WS2812B LED control |
| Adafruit SSD1306 + GFX | OLED display |
| ArduinoJson 7.x | Settings, MessagePack serialization |
| TinyGPSPlus | GPS NMEA parsing |
| NimBLE-Arduino | Bluetooth LE |
| ESPAsyncWebServer | WiFi web interface |
| tinyfsm | State machine support |
| RadioHead (custom fork) | LoRa radio driver |
| ESP32Encoder | Rotary encoder input |
| HMC5883 / QMC5883LCompass | Magnetometer/compass |

## Adding New Features

### New LED Pattern
1. Create header in `include/HelperClasses/LED_Patterns/` inheriting `LED_Pattern_Interface`
2. Implement in `src/HelperClasses/LED_Patterns/`
3. Register with `LED_Manager`

### New Window
1. Create header in `include/HelperClasses/OLED_Window/` inheriting `OLED_Window`
2. Implement states in `include/HelperClasses/Window_States/`
3. Register window with `Display_Manager`

### New LoRa Message Type
1. Inherit from `MessageBase` in `include/HelperClasses/Message_Types/`
2. Implement MessagePack serialization/deserialization
3. Register with `MessageBase::MessageFactory`

### New Draw Command
1. Inherit from `DrawCommandInterface` in `include/HelperClasses/DrawCommands/`
2. Implement `draw()` method

## Time Management

All time queries flow through `System_Utils`. Internally it uses `ezTime` (ropg/ezTime 0.8.3) — once synced, ezTime tracks elapsed time with `millis()`.

> The class is `SystemModule::Utilities`, defined header-only in `include/Utilities/SystemUtilities.hpp` (formerly `System_Utils.h`/`System_Utils.cpp`). The global name `System_Utils` remains available as a backwards-compatible alias (`using System_Utils = SystemModule::Utilities;`).

### TimeSourceInterface

File: `include/Interfaces/TimeSourceInterface.hpp`  
Namespace: `SystemModule::`

```cpp
virtual bool TryGetCurrentUTC(time_t& outTime) = 0;
```

Returns `true` and populates `outTime` (UTC, Unix epoch) on success; `false` if time unavailable. `System_Utils::GetCurrentUTC()` iterates registered sources in order and returns the first success. Each successful call also auto-syncs ezTime's internal clock, so `GetCurrentLocal()` stays accurate without a separate sync step.

Register via `System_Utils::RegisterTimeSource(TimeSourceInterface*)`. Application layer handles wiring during init.

### GpsTimeSource

File: `include/HelperClasses/TimeSource/GpsTimeSource.hpp`  
Header-only. Holds a `TinyGPSPlus&` and checks GPS validity on demand.

```cpp
GpsTimeSource src(NavigationUtils::GetGPS());
System_Utils::RegisterTimeSource(&src);
```

### Adding a New Time Source

1. Create a header-only class at `include/HelperClasses/TimeSource/<ClassName>.hpp`
2. Inherit `SystemModule::TimeSourceInterface`, implement `TryGetCurrentUTC(time_t&)`
3. Register with `System_Utils::RegisterTimeSource()` in app init — sources are tried in registration order, first success wins

### Packed Timestamp Conversion

LoRa messages store TinyGPS++ packed `uint32_t time` (HHMMSSCC) and `uint32_t date` (DDMMYY). These struct fields are not changed.

Convert to `time_t`: `time_t utc = NavigationUtils::PackedToTimeT(msg.time, msg.date)`

### System_Utils Time API

| Method | Description |
|---|---|
| `RegisterTimeSource(ptr)` | Add a time source to the polling list |
| `GetCurrentUTC(time_t&)` | Poll sources; auto-syncs ezTime on success; returns bool |
| `GetCurrentLocal()` | Local `time_t` via `LocalTimezone().now()` |
| `IsTimeValid()` | True if any registered source has valid time |
| `FormatTime(time_t)` | Display string — respects `time24Hour` setting |
| `FormatDate(time_t)` | Date display string |
| `UTCOffset()` | `int&` — Meyers singleton, read/write by reference |
| `LocalTimezone()` | `Timezone&` — Meyers singleton, ezTime timezone object |
| `TimeSources()` | `vector<TimeSourceInterface*>&` — Meyers singleton |

### Meyers Singleton Pattern for Static State

New static state uses function-local statics to avoid `.cpp` definitions:

```cpp
static int& UTCOffset()
{
    static int offset = 0;
    return offset;
}
```

Returns a reference — callers both read (`System_Utils::UTCOffset()`) and write (`System_Utils::UTCOffset() = 5`). No `.cpp` entry needed. This is the required pattern for all new static state in this codebase.

### Timezone Setting

`System_Utils::UTCOffset()` is set by the application when the user changes the timezone setting. Apply the offset to `LocalTimezone()` using a POSIX string (POSIX offsets are sign-inverted from common notation):

```cpp
System_Utils::UTCOffset() = newOffset;
String posix = "UTC" + String(-newOffset);  // UTC+5 local → POSIX "UTC-5"
System_Utils::LocalTimezone().setPosix(posix);
```

### ezTime Quick Reference

| Task | Call |
|---|---|
| Set UTC time | `UTC.setTime(time_t utc)` |
| Get local `time_t` | `System_Utils::LocalTimezone().now()` |
| Build `time_t` from components | `ezt::makeTime(h, m, s, d, mo, yr)` |
| Format for display | `LocalTimezone().dateTime(t, "H:i")` |
| Decompose `time_t` | `ezt::breakTime(t, tmElements_t&)` |

## Navigation Module

### NavigationModule::Utilities

File: `include/Utilities/NavigationUtilities.hpp`  
Fully inline static utility class. No instance needed. Backward-compat alias: `using NavigationUtils = NavigationModule::Utilities`.

All GPS, compass, saved locations, and geolocation source registry functionality lives here. Static state uses Meyers singletons internally — no `.cpp` definitions required.

### NavigationModule::Manager

File: `include/ModuleManagers/NavigationManager.h`  
Instantiated in the application layer. Handles initialization, saved-location persistence to SPIFFS, and calibration data loading. Backward-compat alias: `using NavigationManager = NavigationModule::Manager`.

```cpp
// In app
NavigationModule::Manager navManager;
navManager.InitializeUtils(&compass, Serial2);
```

### GeolocationInterface

File: `include/Interfaces/GeolocationInterface.hpp`  
Namespace: `NavigationModule::`

```cpp
virtual bool TryGetCurrentLocation(double& outLat, double& outLon) = 0;
virtual const char* GetMoniker() const = 0;
```

Returns `true` and populates `outLat`/`outLon` (WGS84 decimal degrees) on success. `NavigationModule::Utilities::GetCurrentLocation()` iterates registered sources in order, first success wins. `GetMoniker()` returns a short identifier for the source (e.g. `"GpsSource"`, `"StaticLocation"`) for debug logging.

### GpsGeolocationSource

File: `include/HelperClasses/Geolocation/GpsGeolocationSource.hpp`  
Header-only. Holds a `TinyGPSPlus&` and checks `location.isValid()` on demand.

```cpp
GpsGeolocationSource src(NavigationModule::Utilities::GetGPS());
NavigationModule::Utilities::RegisterLocationSource(&src);
```

### Adding a New Geolocation Source

1. Create a header-only class at `include/HelperClasses/Geolocation/<ClassName>.hpp`
2. Inherit `NavigationModule::GeolocationInterface`, implement `TryGetCurrentLocation(double&, double&)` and `GetMoniker() const`
3. Register with `NavigationModule::Utilities::RegisterLocationSource()` in app init

### NavigationModule::Utilities Geolocation API

| Method | Description |
|---|---|
| `RegisterLocationSource(ptr)` | Add a geolocation source to the polling list |
| `GetCurrentLocation(double& lat, double& lon)` | Poll sources; returns bool, first success wins |
| `GetCurrentLocation(double& lat, double& lon, std::string& moniker)` | Same, and reports which source's moniker produced the fix |
| `LocationSources()` | `vector<GeolocationInterface*>&` — Meyers singleton |

