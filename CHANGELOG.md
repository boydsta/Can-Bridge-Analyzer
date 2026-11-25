# Changelog

All notable changes to CAN Bridge Analyzer are documented here.

---

## [Unreleased]

- WiFi can join an existing network (router / phone hotspot) instead of running
  its own hotspot; DHCP or static IP; per-unit device name with mDNS
  (`http://<name>.local`)
- Automatic fallback to the own hotspot if the network cannot be joined within
  20 s or is lost; retries the network while nobody is on the hotspot
- Hardware factory reset: AI2 (pin 24) held above 7 V for 200 ms erases all
  settings and reboots to the default hotspot
- Fix: saving `/config` no longer overwrites the active DBC list with
  uninitialised data

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
