# LoRaTwin — dual-mode LoRa receiver and transmitter

Firmware for **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262 + 0.96" OLED) that
receives and transmits LoRa frames compatible with both **MeshCore** and
**Meshtastic** networks.  Includes a web dashboard with live status, event log,
configuration panel, and OTA firmware update.

<img width="981" height="1257" alt="image" src="https://github.com/user-attachments/assets/2c2a0b70-636c-41a4-9d6f-247dc000f010" />


## Features

- **Dual radio profile** — switch between MeshCore (`sync 0x12`, SF8 / BW 62.5 kHz)
  and Meshtastic (`sync 0x2B`, selectable presets SF7–SF12 / BW 62.5–250 kHz)
- **OLED display** — shows active profile, frequency, SF, live RSSI, packet counter,
  channel activity indicators, and history latch
- **Web dashboard** — device status, radio configuration, TX form, event log with
  hex dump, drag-and-drop firmware upload
- **Channel monitoring** — RSSI sampling every 200 ms, energy detection (RSSI
  threshold), CRC fail tracking, activity history latch
- **Event log** — 64-entry circular buffer recording RX, TX, CRC failures, and
  energy-detection events. Viewable on the web UI and Serial
- **OTA updates** — upload new firmware through the web browser (ElegantOTA)

## Hardware

| Interface       | GPIO / Notes                    |
|-----------------|---------------------------------|
| LoRa NSS/CS     | 8                               |
| LoRa SCK        | 9                               |
| LoRa MOSI       | 10                              |
| LoRa MISO       | 11                              |
| LoRa RST        | 12                              |
| LoRa BUSY       | 13                              |
| LoRa DIO1 (IRQ) | 14                              |
| OLED SDA        | 17                              |
| OLED SCL        | 18                              |
| OLED RST        | 21                              |
| Vext (OLED pwr) | 36 (LOW = ON)                   |
| LED             | 35 (active HIGH)                |
| Button          | 0  (active LOW, also BOOT)      |
| I²C addr        | 0x3C                            |

**Important:** GPIO 36 (Vext) must be driven LOW before the OLED receives power.

## Build & flash

### Prerequisites

```powershell
pip install platformio
```

### First upload (USB-C)

1. Edit `src/config.h` — set your WiFi credentials:
   ```cpp
   #define WIFI_SSID "YourSSID"
   #define WIFI_PASS "YourPassword"
   ```
2. Build, flash firmware and filesystem:
   ```powershell
   cd D:\kody\heltec
   pio run -e heltec_wifi_lora_32_V3 -t upload -t uploadfs --upload-port COM3
   ```
3. Open Serial monitor:
   ```powershell
   pio device monitor --port COM3
   ```

### OTA update (WiFi)

Browse to `http://<device-ip>/` and drag the `.bin` file onto the upload zone,
or use `http://<device-ip>/update` (ElegantOTA).

## Default radio profiles

| Parameter        | MeshCore          | Meshtastic        |
|------------------|-------------------|-------------------|
| Frequency (EU)   | 869.618 MHz       | 869.525 MHz       |
| Sync word        | `0x12`            | `0x2B`            |
| Spreading factor | 8 (fixed)         | 7–12 (preset)     |
| Bandwidth        | 62.5 kHz (fixed)  | 62.5–250 kHz      |
| Coding rate      | 4/8               | 4/8               |
| Preamble         | 16                | 16                |

Switch profiles on the web dashboard — the radio re-initialises immediately.

## Bugs encountered & fixed

### 1. `readData()` buffer-length overflow

**Symptom:** frames received but CRC always failed; sometimes no frames at all.

**Root cause:** `pkt.len` is `uint8_t` (max 255) but `sizeof(pkt.data)` returns
256, which overflows to 0.  RadioLib's `readData()` was called with length 0 and
a type-mismatched reference (`uint8_t&` vs the expected `size_t&`).

**Fix:** use a local `size_t len` variable for the buffer length, then truncate
to `uint8_t` only after `readData()` returns the actual payload size.

### 2. TCXO voltage — frequency offset at narrow bandwidth

**Symptom:** MeshCore frames (BW 62.5 kHz) always failed CRC, while Meshtastic
frames (BW 125–250 kHz) were received correctly.

**Root cause:** the Heltec V3 SX1262 uses a TCXO for frequency stability. The
correct control voltage varies between board revisions — some use 1.6 V, others
1.8 V.  At 62.5 kHz bandwidth even a small frequency offset corrupts bits.  With
1.8 V on a 1.6 V board, the offset was large enough to break every packet.

**Fix:** set TCXO voltage to 1.6 V in `LORA_TCXO_VOLTAGE` (`config.h`).  If your
board requires 1.8 V, change it back — the symptom is clear: clean RX on wide
bandwidth, CRC-only on narrow bandwidth.

### 3. SPI pin mapping on ESP32-S3

**Symptom:** `radio.begin()` returns error code −2 (`RADIOLIB_ERR_SPI_CMD_FAILED`).

**Root cause:** the Heltec V3 board definition in some PlatformIO releases has
incorrect default SPI pins for the SX1262 (e.g., SS=10, SCK=12 instead of the
correct SS=8, SCK=9).

**Fix:** explicitly construct a dedicated `SPIClass` with the correct pins and
pass it to the RadioLib `Module` constructor.

### 4. OLED stays dark

**Symptom:** display never turns on, even though I²C communication succeeds.

**Root cause:** GPIO 36 (Vext) powers the OLED through a MOSFET. It must be
driven LOW to enable the 3.3 V rail.

**Fix:** `pinMode(36, OUTPUT); digitalWrite(36, LOW);` before initialising
the display.

### 5. NVS settings migration after profile support was added

**Symptom:** after firmware update, device boots with wrong frequency for the
selected profile (e.g., MeshCore listening on 869.525 MHz instead of
869.618 MHz).

**Root cause:** old NVS stored `freq`/`preset`/`txpower` but not the new
`profile` key.  `settings_load()` used the stored frequency instead of the
MeshCore default.

**Fix:** check for the presence of the `profile` key. If missing, reset all
settings to the current firmware defaults and persist them.

### 6. ESPAsyncWebServer fork incompatibility

**Symptom:** build errors in `ESPAsyncWebServer` with ESP32-S3 Arduino 3.x.

**Root cause:** the popular `esphome/ESPAsyncWebServer` fork was deleted.
The `me-no-dev` original and `mathieucarbou` fork had const-correctness
issues and dependency conflicts with ElegantOTA.

**Fix:** let ElegantOTA pull in its own compatible `esp32async/ESPAsyncWebServer`
transitive dependency. Added `-fpermissive` to tolerate minor const-correctness
warnings in the library.

### 7. `startReceive()` failure silently kills reception

**Symptom:** device receives a few frames, then stops — no more packets, no
errors logged. After reboot, works briefly then stops again.

**Root cause:** in `lora_process()`, after reading a packet (or CRC fail),
`radio.startReceive()` was called without checking the return value. If that
SPI call fails (BUSY timeout, SPI glitch, radio in bad state), the SX1262
stays in standby and never re-enters RX mode. The ISR still fires on DIO1
edges but `readData()` returns garbage since the radio isn't receiving.
Same issue in the warmup path and in `lora_tx()`.

**Fix:** check `startReceive()` return value everywhere. On failure, call
`lora_reinit()` to fully restart the radio. This recovers from transient
SPI or radio state errors automatically.

### 8. `activityUntil` not set for energy detection

**Symptom:** energy detection indicator on OLED never shows activity, even
when RSSI clearly exceeds the threshold.

**Root cause:** when RSSI crossed `RSSI_ACTIVITY_THRESHOLD`, `activityState`
was set to `ACT_ENERGY` but `activityUntil` was not updated. The hold timer
check (`now > activityUntil`) used a stale/zero value and immediately cleared
the state back to `ACT_IDLE`.

**Fix:** set `activityUntil = now + ACTIVITY_HOLD_MS` when entering
`ACT_ENERGY`, same as for `ACT_RECEIVING` and `ACT_CRC_FAIL`.

### 9. TOCTOU race on `packetReceived` flag

**Symptom:** occasional lost packets, especially under heavy traffic.

**Root cause:** `packetReceived` is set by ISR (DIO1 rising edge) and
cleared by the main loop. Between the `if (!packetReceived) return` check
and `packetReceived = false` clear, the ISR can fire for a new packet.
The main loop then clears the flag, and the second packet is never read.

**Fix:** wrap the read + clear in a critical section — detach the DIO1
interrupt before clearing the flag and reading data, then reattach after
`startReceive()` succeeds. This ensures no ISR can fire between the
check and clear.

### 10. SPI hardware SS conflicts with RadioLib manual NSS

**Symptom:** intermittent SPI failures (`RADIOLIB_ERR_SPI_CMD_FAILED`),
especially after radio reinit (profile switch or settings change).

**Root cause:** `loraSpi.begin(SCK, MISO, MOSI, NSS)` passed the LoRa NSS
pin as the SPI controller's hardware SS. The ESP32 SPI driver can
automatically assert/deassert the SS pin, which conflicts with RadioLib's
manual NSS control via `digitalWrite()`.

**Fix:** pass `-1` as the SS parameter to `loraSpi.begin()` so the SPI
hardware never touches the NSS line.

### 11. No recovery from stuck radio state

**Symptom:** after a transient error, radio never receives again until
manual reboot.

**Root cause:** no watchdog or health-check mechanism. If the radio enters
a bad state (e.g., `startReceive()` fails), the only recovery path was a
manual reset.

**Fix:** `lora_process()` and `lora_tx()` now detect `startReceive()`
failures and automatically call `lora_reinit()` to restart the radio.
This handles transient SPI errors, BUSY pin lockups, and unexpected
radio state transitions.

### 12. TOCTOU race on `packetReceived` flag (actually fixed)

**Symptom:** occasional lost packets, especially under heavy traffic.

**Root cause:** `packetReceived` is set by ISR (DIO1 rising edge) and
cleared by the main loop. Between the `if (!packetReceived) return` check
and `packetReceived = false` clear, the ISR can fire for a new packet.
The main loop then clears the flag, and the second packet is never read.

**Fix:** wrap the read + clear in a critical section — detach the DIO1
interrupt before clearing the flag and reading data, then reattach after
`startReceive()` succeeds. This ensures no ISR can fire between the
check and clear. (Previously documented as fixed but not implemented.)

### 13. Blocking `delay(30)` in RX path

**Symptom:** under moderate to heavy traffic, packets are missed and RSSI
sampling becomes irregular.

**Root cause:** after successful packet reception, the code called
`delay(30)` to blink the LED. This blocks the main loop for 30 ms per
packet — during this time no new packets can be processed, no RSSI
sampling occurs, and the ISR flag for subsequent packets is set but not
handled until the delay returns.

**Fix:** replaced with non-blocking LED blink using timestamp tracking
(`ledBlinkUntil`, `ledBlinkActive`). The LED is turned on immediately,
and the main loop turns it off after 30 ms without blocking.

### 14. Packet queue overflow (silent drop)

**Symptom:** burst traffic causes packets to disappear without any error
indication.

**Root cause:** the packet queue (`PacketRing`) had a capacity of only 16
entries. The display task pops packets at ~10 Hz (every 100 ms). If
packets arrive faster than ~10/s, the queue fills and `push()` returns
`false` — packets are silently dropped.

**Fix:** increased queue capacity from 16 to 64 entries. Added `volatile`
qualifier to `currentRssi` for safe sharing between RSSI sampling and
packet reception paths.

## Project structure

```
heltec/
├── platformio.ini                  # Build config
├── partitions_8mb_dual_ota.csv     # 8 MB dual-OTA partition table
├── data/
│   └── index.html                  # Web dashboard
├── src/
│   ├── main.cpp                    # Entry point
│   ├── config.h                    # All constants (WiFi, pins, radio)
│   ├── lora_handler.h / .cpp       # SX1262 init, ISR, RX/TX, activity monitor
│   ├── display_handler.h / .cpp    # OLED rendering (U8g2)
│   ├── settings.h / .cpp           # NVS-backed radio settings
│   ├── web_server.h / .cpp         # HTTP server, REST API, ElegantOTA
│   └── wifi_manager.h / .cpp       # WiFi connect / reconnect
└── README.md
```

## Web API

| Method | Path          | Description                                  |
|--------|---------------|----------------------------------------------|
| GET    | `/api/status` | Device status (chip, uptime, RSSI, packets…) |
| GET    | `/api/config` | Current radio configuration                  |
| POST   | `/api/config` | Update radio config (profile, freq, preset)  |
| POST   | `/api/tx`     | Transmit a LoRa frame                        |
| GET    | `/api/log`    | Event log (last 64 entries)                  |
| GET    | `/update`     | ElegantOTA firmware upload page              |

## License

MIT — software by SP3FHI
