# Changelog

All notable changes to CAN Bridge Analyzer are documented here.

---

## [0.2.0] — 2026-05-03

### Added
- **Signal age colour coding** — message items in the left and right bus panels now show a coloured left-border based on activity:
  - 🟢 Green — payload changed since last WebSocket push (active, changing)
  - 🔵 Blue — data present but payload stable (active, static)
  - ⬜ Grey + dimmed — no update received in the last 2 seconds (stale)
- **Novel-event detection** — an amber 🟠 left-border highlights signals that are not normally changing but suddenly do, ignoring high-churn signals (e.g. RPM, speed).  A rolling 30-sample churn window classifies each ID; if fewer than 20 % of recent samples saw a payload change the ID is considered stable.  Any change on such an ID — or a brand-new CAN ID — triggers a 15-second amber highlight and a brief flash animation.  Useful for spotting exactly which IDs wake up when you plug in an EVSE, press the brake, or trigger any other discrete event on a quiet bus.
- **Periodic message-list refresh** — a 3-second timer re-renders both bus panels so newly seen CAN IDs appear without a manual page reload.

### Removed
- **Filter input** — the ID/description filter bar in the control panel has been removed to reduce UI clutter. Sorting by ID, period, count, or activity is still available.
- **Clear / Export buttons** — removed from the main header controls.
- **Send Custom Frame panel** — removed completely from the center analysis panel.
- **Capture snapshot button** — removed from the Testing Controls panel.
- **Copy payload button** — removed from the Testing Controls panel.

---

## [0.1.0] — 2026-04-xx (initial release)

- Dual-bus CAN 2.0B / CAN FD capture via ACAN2517FD
- Real-time WebSocket dashboard with per-ID statistics
- Bidirectional CAN bridge with per-ID block list
- DBC file upload and browser-side signal decode
- TX Scheduler with NVS persistence and rolling-counter support
- SavvyCAN CSV and binary GVRET serial output modes
- Runtime configuration via `/config` page (speed, WiFi, bus enable)
- Marker / snapshot page for comparative capture sessions
