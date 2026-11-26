# Changelog

All notable changes to CAN Bridge Analyzer are documented here.

---

## [0.4.0] — 2026-11-27

> **Flash over USB once:** the partition table has changed. Do a full
> `-t upload` and `-t uploadfs`; DBC files uploaded through the web UI are lost.
> After that, all updates can be done over WiFi.

### Firmware updates over WiFi
- OTA updates at `/update` (CustomOTA v1.6.1): firmware and DBC filesystem
  images, with rollback to the previous firmware. Linked from the dashboard
  and the Config page
- Updates, reboot and rollback are blocked while CAN traffic is flowing; they
  unlock once both buses have been quiet for 3 s
- New partition table (`partitions.csv`): two 1.625 MB firmware slots and
  704 KB SPIFFS for DBC files

### WiFi
- WiFi can join an existing network (router / phone hotspot) instead of running
  its own hotspot; DHCP or static IP; per-unit device name with mDNS
  (`http://<name>.local`)
- Automatic fallback to the own hotspot if the network cannot be joined within
  20 s or is lost; retries the network while nobody is on the hotspot
- Hardware factory reset: AI2 (pin 24) held above 7 V for 200 ms erases all
  settings and reboots to the default hotspot
- Fix: saving `/config` no longer overwrites the active DBC list with
  uninitialised data

### Serial output
- Leftover bring-up output removed (bit-timing dumps, SUCCESS/FAILED banners,
  board/CPU info); each bus now prints one init result line
- Blocked and failed-to-forward frames are counted in the 10 s bridge stats
  instead of printing a line (and register dump) per frame

### DBC files
- Nissan Leaf DBCs updated to dalathegreat/leaf_can_bus_messages `6eca0f8`:
  CHAdeMO voltages now decode correctly (1 V/bit, Intel), 0x5BC capacity
  multiplexing reworked, VSP messages 0x559 and 0x606 added

### Documentation and licensing
- User manual (`manual/`) with SPUD wiring diagram (pins, left/right panel)
- Config page CAN0/CAN1 headings show panel and connector pins
- Open source notices: `THIRD_PARTY_LICENSES.md`, on-device `/licenses` page
  with firmware version and commit, full licence texts built into the firmware
- spud-diy (MIT, © 2024 Ra Cleave) credited; original fork of the Leaf DBCs
  (baradhili) credited
- README lists the supported controllers: MCP251863 and MCP2515

---

## [0.3.0] — 2026-11-26 — First public release

- Dual-bus CAN 2.0B / CAN FD capture on the SPUD board (MCP251863 via ACAN2517FD)
- Alternative build for classic-CAN MCP2515 hardware (CAN 2.0 only)
- Real-time WebSocket dashboard with per-ID statistics
- Signal activity colour coding (changing / static / stale) and novel-event
  highlighting for IDs that suddenly change on a quiet bus
- Bidirectional CAN bridge with per-ID block list
- DBC file upload and browser-side signal decode; Nissan Leaf DBCs pre-loaded in SPIFFS
- TX Scheduler with NVS persistence and rolling-counter support
- SavvyCAN CSV and binary GVRET serial output modes
- Runtime configuration via `/config` page (bus enable, speed, WiFi, serial mode)
- Marker / snapshot page for comparative capture sessions
