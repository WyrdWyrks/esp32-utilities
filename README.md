# esp32-utilities

A hardware-focused **ESP32 library** that provides high-level, reusable building blocks for embedded
projects — OLED UI, addressable-LED animation, LoRa mesh networking, GPS/compass navigation, settings
persistence, and an RPC framework. It is designed to be **consumed by an application layer project**
that wires the managers to concrete hardware and supplies app-specific windows, messages, and patterns.

The library originated with the [Celestial Wayfinder](https://github.com/Blake-Ballew/Celestial-Wayfinder)
project and is being progressively decoupled into a general-purpose toolkit.

> **For contributors / AI agents:** [`CLAUDE.md`](CLAUDE.md) is the authoritative deep-dive on coding
> conventions, the time/navigation APIs, and the patterns to follow when adding code. This README is the
> high-level tour; `CLAUDE.md` is the reference manual.

---

### Key dependencies

| Library | Purpose |
|---|---|
| FastLED | WS2812B / addressable LED control |
| Adafruit GFX | OLED display |
| ArduinoJson 7.x | Settings + MessagePack serialization (elastic `JsonDocument`) |
| TinyGPSPlus | GPS NMEA parsing (accessed via `GeolocationInterface`) |
| Magnetometer / compass | App-supplied driver, accessed via `CompassInterface` |
| ESPAsyncWebServer | WiFi web interface / RPC-over-HTTP |
| NimBLE-Arduino | Bluetooth LE GATT server (RPC transport) |
| ezTime | Time tracking / formatting |
| mbedTLS (ESP-IDF) | AES-128 + PBKDF2 for LoRa payload encryption |

---

## Architecture

The library is layered. Applications depend on **Module Managers**; managers compose **Helper Classes**,
which build on **Utilities** and **Interfaces**.

```
Application Project
    ↓
Module Managers   (Display, LED, Lora, Navigation, Filesystem, EspNow, Rpc)
    ↓
Helper Classes    (Windows, States, Layers, LED Patterns, Messages, Draw Commands)
    ↓
Utilities
    ↓
Interfaces
```

Most cross-module communication runs through two primitives in `SystemModule::Utilities`: **FreeRTOS
tasks/queues/timers** (everything that needs a thread or buffered hand-off registers here and refers to
its resource by integer ID) and the generic **`EventHandler<Args...>`** multicast delegate (used for
`onRenderComplete`, `SettingsUpdated`, `systemShutdown`, per-LoRa-schema message events, etc.).

### Recurring patterns

- **Singleton managers** — class-level state, FreeRTOS-safe.
- **Module Utilities + Module Manager split** — `Utilities` holds the static state and logic; `Manager`
  owns hardware init and the FreeRTOS task loop. (e.g. `DisplayModule::Utilities` + `DisplayModule::Manager`.)
- **Meyers / `inline static` singletons** — new static state lives in headers, so there are no `.cpp`
  definitions to maintain. See `CLAUDE.md`.
- **Pluggable source interfaces** — `TimeSourceInterface`, `GeolocationInterface`, `CompassInterface`,
  `LoraDriverInterface`, `NetworkStreamInterface` let the app inject hardware without touching the library.
- **Queue-driven dispatch** — input events and display commands flow through FreeRTOS queues registered
  with `System_Utils`.

### 🚧 Ongoing refactor

The codebase is **mid-migration**, so you will see two generations of many components side by side:

| Legacy (being retired) | Current (preferred) |
|---|---|
| `Display_Manager.h`, `LoraManager.h`, `NavigationUtils.h` | `DisplayManager.hpp`, `LoraManager.hpp`, `NavigationUtilities.hpp` |
| `HelperClasses/Window_States/*.h` | `HelperClasses/Window/States/*.hpp` |
| Global classes (`LED_Utils`, `System_Utils`) | Namespaced modules (`SystemModule::`, `LoraModule::`, …) |
| `.cpp` implementations | Header-only, `inline static`, Meyers singletons |

**New code goes in namespaced, header-only `.hpp` files.** Backward-compatible aliases
(`using System_Utils = SystemModule::Utilities;`, `using NavigationUtils = NavigationModule::Utilities;`)
keep existing call sites working during the transition.

---

## Modules

### Display (`DisplayModule`)

OLED UI orchestration. `Manager` runs a single FreeRTOS task that drains the display-command queue and
drives rendering; `Utilities` holds the window stack and the **layer render pipeline**.

- **Layers** — each frame, registered `LayerInterface` implementations draw in ascending `LayerID` order:
  `CONTENT` (state draw commands) → `WINDOW` (window chrome + input labels) → `USER_BASE` (app HUDs,
  edge compass, etc.). Layers can be disabled at runtime without leaving the pipeline.
- **Windows & States** — a `Window` owns a stack of `WindowState`s. States map inputs to commands, switch
  states, push/pop, and can hand `StateTransferData` to the next state (so editors like text/number entry
  are shared across windows). Windows hold no parent pointer and no direct `Adafruit_GFX` dependency —
  they draw through a `DrawContext`.
- **Draw Commands** — small `DrawCommandInterface` primitives (`TextDrawCommand`, battery/bell/message/WiFi
  icons, QR code, `FnDrawCommand` for inline lambdas).
- Stock windows: Menu, Settings, GPS, Save/Edit Saved Locations, Diagnostics, OTA Update.

### LED (`LED_Utils` / `LED_Manager`)

Addressable-LED (WS2812B / FastLED) animation engine. Patterns implementing `LedPatternInterface` are
registered with `LED_Utils`, which animates them on a shared FreeRTOS task (default 50 ms/frame),
supporting finite/infinite looping, per-pattern config (via `JsonDocument`), per-pattern enable/disable,
and a global theme color. Bundled patterns: `SolidRing`, `RingPulse`, `RingPoint`, `RingShutdown`,
`ScrollWheel`, `ButtonFlash`, `IlluminateButton` — built around a compass LED ring plus button LEDs.
`LED_Manager` bootstraps FastLED and the pattern task and also owns the **discrete haptics outputs** —
buzzer tones and a non-blocking (timer-driven) haptic motor pulse on `HARDWARE_VERSION >= 3`.

### LoRa mesh (`LoraModule`)

LoRa mesh networking over a pluggable `LoraDriverInterface`. `Manager::RadioTask` blocks on a task
notification driven by the radio's DIO0 ISR (receive) and the send queue (transmit). Messages implement
`LoraMessageInterface`, are serialized to **MessagePack** (ArduinoJson), and are constructed
polymorphically by a schema-hash (FNV-1a over sorted payload keys) message factory; per-schema
`EventHandler`s dispatch decoded messages to the app. The mesh layer is built for real RF conditions:

- **Two-phase receive** — base routing fields (sender, msg ID, bounces-left) are read *before* decryption,
  so a node can route/relay traffic for other "chatrooms" it cannot decrypt.
- **Encrypted "chatrooms"** — payloads are AES-128 encrypted (see Security below); a non-matching key
  yields `nullptr` on deserialize, but the raw ciphertext is still relayed intact.
- **Loop & flood control** — echo detection (drops self-originated messages), per-sender duplicate
  suppression, and a bounce/TTL counter clamped to `MAX_BOUNCES_LEFT`.
- **RSSI-based relay backoff** — weaker received signal ⇒ shorter forwarding delay, so distant
  (better-positioned) relays transmit first; transmit waits for a clear channel.

### Navigation (`NavigationModule`)

GPS + compass integration. `Utilities` (fully inline/static) owns the `TinyGPSPlus` instance, compass via
`CompassInterface`, saved locations, and a registry of `GeolocationInterface` sources (first valid source
wins). `Manager` handles LittleFS persistence of saved locations and compass calibration. Feeds the LED
compass ring to point at a target coordinate. See `CLAUDE.md` for the full geolocation/source API.

### System (`SystemModule::Utilities`, alias `System_Utils`)

The device-wide service layer (header-only) every other module builds on. Provides:
- **FreeRTOS registries** — tasks (dynamic/static, optionally core-pinned), queues, and timers, each
  referred to by an integer ID so other modules don't hold raw handles.
- **Time** — ezTime-backed services with a registry of `TimeSourceInterface` sources (`GpsTimeSource`,
  `EzTimeSource`) plus timezone/formatting helpers (see the **Time Management** section of `CLAUDE.md`).
- **Battery** — app-supplied percentage callback and a `systemShutdown` event (e.g. low-battery → play
  shutdown animation, then cut power, ordered via `EventHandler::PushFront`).
- Device identity (`DeviceName`, `DeviceID`), OTA, CRC, and base64 helpers.

### Filesystem & Settings (`FilesystemModule`)

LittleFS file I/O — all data is stored as **MessagePack** — plus the device **settings system**. Settings
are typed objects (`Bool/Int/Float/String/Enum`) behind `SettingsInterface`, held in a `SettingsMap`,
persisted to NVS via `Preferences`, and editable on-device through the Settings window or remotely via
RPC (`RpcGetSettingsFile`, `RpcUpdateSetting(s)`). A `SettingsUpdated` event lets any module react to
changes (e.g. `BluetoothUtilities::SettingsUpdated`, timezone updates).

### Connectivity & Radio (`ConnectivityModule`)

WiFi / ESP-NOW transport and provisioning:
- **`RadioUtils`** — a single-owner WiFi radio state machine (`OFF / STA / AP / ESP-NOW / BT`) with helpers
  for SmartConfig, AP mode, ESP-NOW init, and channel control (the ESP32 radio can only be in one mode at
  a time).
- **`EspNowManager`** — ESP-NOW peer messaging; `ConnectivityUtils` layers an RPC-over-ESP-NOW queue on top.
- **WiFi provisioning** — none / ESP-NOW / temporary-AP modes, driven from settings.
- **`IpUtils`** — registry of `NetworkStreamInterface` streams (`WiFiTcpStream`, `WiFiUdpStream`,
  `TcpConnectionBroadcast`) and the glue that binds an RPC channel to a network stream.

### Bluetooth LE (`BluetoothModule` / `BluetoothUtilities`)

NimBLE GATT server exposing the device for remote control. Requires **secure pairing** (bonding + MITM
protection, with a passkey shown on-screen) and serves an **RPC characteristic**: incoming MessagePack
requests are reassembled from BLE-sized chunks, dispatched through `RpcModule`, and the response is
chunked back out on read. Because the ESP32 can't run WiFi and BT simultaneously, init disables WiFi first.

### RPC (`RpcModule`)

A lightweight gRPC-style call framework over MessagePack. Functions are registered by name with
`RpcModule::Utilities`; `Manager` runs a task that polls every registered `RpcChannel`, dispatches the
named function, and (if the channel supports replies) sends a structured return code (`RPC_SUCCESS`,
`RPC_FUNCTION_NOT_REGISTERED`, `RPC_FUNCTION_ERROR`, …). The same function registry is reachable over
**multiple transports**: USB serial (`RPC-->` / `RPC<--` framing), an async **web server** (`POST /rpc`
with msgpack, plus `/ping` and an info `/` route), **BLE** (above), and **network streams** (TCP/UDP).

### Security (`EncryptionUtils`)

AES-128-CBC with PKCS7 padding for LoRa payloads. Keys are derived from a passphrase via
PBKDF2-HMAC-SHA256 (fixed app salt, 10 000 iterations) so every device sharing a passphrase derives the
same key — the basis for encrypted LoRa "chatrooms". IVs come from the ESP32 hardware RNG. Built on
ESP-IDF mbedTLS.

---

## Project structure

```
esp32-utilities/
├── include/                       # Public headers (header-only is preferred for new code)
│   ├── Interfaces/                # Pluggable interfaces (TimeSource, Geolocation, Compass,
│   │                              #   LoraDriver, NetworkStream, Layer, Window, DrawCommand, …)
│   ├── ModuleManagers/            # Top-level managers (Display, LED, Lora, Navigation,
│   │                              #   Filesystem, EspNow, Rpc)
│   ├── HelperClasses/
│   │   ├── DrawCommands/          # OLED drawing primitives
│   │   ├── LED/Patterns/          # LED animation patterns
│   │   ├── Layers/                # Render-pipeline layers
│   │   ├── Window/  + Window/States/   # Current windows & state machine
│   │   ├── Window_States/         # Legacy states (being migrated)
│   │   ├── Message_Types/         # LoRa message definitions
│   │   ├── Network/               # WiFi/TCP/UDP stream classes
│   │   ├── GPS/ · TimeSource/     # Concrete geolocation & time sources
│   │   └── Rpc/                   # RPC channel
│   └── Utilities/                 # System, Navigation, Lora, LED, Display, Filesystem/Settings,
│                                  #   Connectivity, Radio, Ip, Bluetooth, Encryption, Event, Version
└── src/                           # Remaining .cpp implementations (Navigation, LED, Filesystem utils)
```

---

## Extending the library

| To add… | Do this |
|---|---|
| **LED pattern** | Implement `LedPatternInterface` in `include/HelperClasses/LED/Patterns/`, register with `LED_Utils`. |
| **Window** | Subclass `DisplayModule::Window`, implement its states under `Window/States/`, register the window factory. |
| **LoRa message** | Implement `LoraMessageInterface`, add MessagePack (de)serialization, register with the message factory. |
| **Draw command** | Implement `DrawCommandInterface`'s `draw()` in `include/HelperClasses/DrawCommands/`. |
| **Time / Geolocation source** | Implement `TimeSourceInterface` / `GeolocationInterface` (header-only), register at app init. |
| **Render layer** | Implement `LayerInterface`, register at a `USER_BASE+` `LayerID`. |

Follow the conventions in [`CLAUDE.md`](CLAUDE.md): namespaced module, header-only, `inline static` /
Meyers-singleton static state, braces on all branches, `ESP_LOG` for logging, C++17.

---

## Roadmap

- Finish the namespaced, header-only migration (retire legacy `.h` / `Window_States` duplicates).
- Continue decoupling from the Celestial Wayfinder application layer.
- Expand the RPC framework and network transports.
</content>
</invoke>
