# CAN Bridge Analyzer

A dual-bus CAN 2.0 / CAN FD capture, analysis, and bridging tool for the **SPUD** DIY controller board by [Ripple Tech](https://shop.rippletech.co.nz).

Flash it once and configure everything over WiFi — no re-flashing required to change bus speed, mode, or SSID.

---

## Hardware

This firmware is designed for the **SPUD v1.1** board from [Ripple Tech](https://shop.rippletech.co.nz), which features:

- ESP32 microcontroller
- Two MCP2517FD / MCP251863 CAN FD controllers connected via SPI
- 40 MHz oscillator for each CAN controller
- Dual isolated CAN transceivers (CANH / CANL)

> The SPUD board is available from [shop.rippletech.co.nz](https://shop.rippletech.co.nz)

---

## Features

- **Dual-bus CAN capture** — monitor CAN0 and/or CAN1 simultaneously
- **CAN 2.0B and CAN FD** — select bus speed and frame type at runtime
- **WiFi web interface** — real-time dashboard at `http://192.168.1.1`
- **SavvyCAN compatible** — serial output in CSV or binary GVRET format
- **CAN bridge mode** — bidirectional message forwarding between CAN0 and CAN1
- **Per-ID filtering** — block individual CAN IDs from forwarding via the web UI
- **Bus health diagnostics** — detailed error counter reporting with actionable guidance
- **NVS configuration** — all settings persist across reboots

---

## Getting Started

### 1. Flash the firmware

```bash
# Install PlatformIO then:
pio run --target upload
```

### 2. Connect to WiFi

After booting, the device creates a WiFi access point:

| Setting  | Default             |
|----------|---------------------|
| SSID     | `CanBridgeAnalyzer` |
| Password | `canbus123`         |

Connect your phone or laptop to this network.

### 3. Open the web interface

Navigate to **http://192.168.1.1** in any browser.

### 4. Configure the device

Go to **http://192.168.1.1/config** to set:

- CAN bus enable (CAN0, CAN1, or both)
- Bus speed and frame type (125k–1M CAN 2.0 or CAN FD up to 8 Mbps data rate)
- Serial output mode (diagnostics / SavvyCAN CSV / binary GVRET)
- WiFi SSID and password

Changes are saved to flash and take effect after reboot.

---

## Serial Logging

Connect a USB serial monitor at **115200 baud**.

| Mode | Description |
|------|-------------|
| 0 — Diagnostics | Bus health, error counters, performance stats |
| 1 — SavvyCAN CSV | Save output to `.csv` and import via *File → Load Log File → GVRET Logs* |
| 2 — Binary GVRET | Live connection in SavvyCAN via *Add New Device → GVRET Serial* |

Toggle serial logging on/off from the web UI without rebooting.

The `rev2gvret.py` script (included) can convert raw serial captures to GVRET format for offline SavvyCAN import.

---

## CAN Bus Wiring

| Signal | SPUD Pin |
|--------|----------|
| CAN0 CANH / CANL | CAN0 connector |
| CAN1 CANH / CANL | CAN1 connector |

- Use **120 Ω termination resistors** at each physical end of the CAN cable.
- With a single node on the bus, plug a 120 Ω resistor across CANH–CANL on the connector.
- Maximum cable length at 500 kbps ≈ 10 m; at 1 Mbps ≈ 5 m.

---

## Nissan Leaf CAN IDs

`src/can_ids.h` contains commented CAN ID definitions for the Nissan Leaf ZE0/AZE0 BMS (2011–2017) as a reference example for decoding real-world CAN traffic.

---

## Building

Requires [PlatformIO](https://platformio.org/). Dependencies are fetched automatically on first build.

```bash
pio run          # build
pio run -t upload  # build and flash
pio device monitor  # open serial monitor
```

---

## License

MIT — see individual library folders for their respective licences.

---

## Links

- SPUD board: [shop.rippletech.co.nz](https://shop.rippletech.co.nz)
- SavvyCAN: [https://www.savvycan.com](https://www.savvycan.com)
- ACAN2517FD library: [github.com/pierremolinaro/ACAN2517FD](https://github.com/pierremolinaro/ACAN2517FD)
