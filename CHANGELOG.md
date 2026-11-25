# Changelog

All notable changes to CAN Bridge Analyzer are documented here.

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
