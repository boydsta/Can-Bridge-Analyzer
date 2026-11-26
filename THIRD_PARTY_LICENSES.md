# Third-Party Licenses

CAN Bridge Analyzer's own source code is MIT licensed (see [LICENSE](LICENSE)).
The compiled firmware and the SPIFFS image also contain the third-party
components below, each under its own license.

## Compiled into the firmware

| Component | License | Source | Notes |
|-----------|---------|--------|-------|
| spud-diy (`src/spud.h`, parts of `src/main.cpp`) | MIT, © 2024 Ra Cleave | https://github.com/racleave/spud-diy | Notice kept in `src/spud.h` |
| ACAN2517FD | MIT, © 2018 Pierre Molinaro | https://github.com/pierremolinaro/acan2517FD | Vendored in `lib/ACAN2517FD` with local patches ([PATCHES.md](lib/ACAN2517FD/PATCHES.md)) |
| CustomOTA-ESP32 v1.6.1 | MIT, © 2025 Boyd Benton | https://github.com/boydsta/CustomOTA-ESP32 | |
| AsyncTCP (mathieucarbou fork) ^3.2.3 | LGPL-3.0 | https://github.com/mathieucarbou/AsyncTCP | |
| ESPAsyncWebServer (mathieucarbou fork) ^3.1.1 | LGPL-3.0 | https://github.com/mathieucarbou/ESPAsyncWebServer | |
| MCP_CAN_lib (MCP2515 build only) | LGPL-3.0 | https://github.com/coryjfowler/MCP_CAN_lib | |
| Arduino core for ESP32 (espressif32@6.10.0) | LGPL-2.1 | https://github.com/espressif/arduino-esp32 | |
| ESP-IDF | Apache-2.0 | https://github.com/espressif/esp-idf | Used by the Arduino core |

The LGPL libraries are statically linked. You can modify them and rebuild the
firmware from this repository with PlatformIO (`pio run -e <env>`), then
install it over USB (`pio run -e <env> -t upload`) or over WiFi at
`http://192.168.1.1/update`.

## Data files in the SPIFFS image

| Files | License | Source |
|-------|---------|--------|
| `data/CAR-can_AZE0.dbc`, `EV-can_AZE0.dbc`, `EV-can_ZE0.dbc`, `EV-can_ZE1.dbc`, `QC-CAN_ALL.dbc` | GPL-3.0 | https://github.com/dalathegreat/leaf_can_bus_messages (forked from https://github.com/baradhili/leaf_can_bus_messages) |
| `data/SPUD-Dash.dbc` | MIT | This project |

The GPL-3.0 DBC files are unmodified, separate data files loaded at runtime;
they are not part of the MIT firmware. See [data/README.txt](data/README.txt)
and [data/LICENSE-GPL-3.0.txt](data/LICENSE-GPL-3.0.txt).

## License texts

- MIT: [LICENSE](LICENSE)
- GPL-3.0: [data/LICENSE-GPL-3.0.txt](data/LICENSE-GPL-3.0.txt)
- LGPL-3.0: [licenses/LGPL-3.0.txt](licenses/LGPL-3.0.txt)
- LGPL-2.1: [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt)
- Apache-2.0: [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)

All of the above, with every copyright notice, are also built into the
firmware and shown on the device at `http://192.168.1.1/licenses`.
