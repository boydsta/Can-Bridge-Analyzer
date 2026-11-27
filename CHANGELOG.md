# Changelog

All notable changes to CAN Bridge Analyzer are documented here.

---

## [0.5.0] — 2026-11-28

> **Updates over the air from 0.4.0** (`/update`, `firmware.bin`): the partition
> table is unchanged. Saved settings are kept.

### Serial logging
- Removed the per-bus "Print frames to serial" tick boxes. They were saved but
  never used; both buses are always logged
- Removed the per-bus Enable settings: both controllers always run (CAN1 used
  to default to off). An unwired bus simply receives nothing
- TX Scheduler frames are now logged, marked `Tx` in the CSV `Dir` column
- Binary GVRET: logging starts by itself when SavvyCAN connects
- Frame writes from different tasks no longer interleave mid-line
- Removed the unused `/api/serial_log_format` endpoint

### Web UI
- Status badge on each bus panel: Live / No traffic / No ACK / RX errors /
  Bus off / No controller, with error counters on hover (`/api/bus_status`)
- Dashboard header slimmed down: Theme, DBC and OTA moved to the Config page;
  Config sits at the far right
- Config page simplified: Serial logging, Bus speed, WiFi, then DBC files
  (shows active files) and Firmware cards; shorter text, one save confirmation
- CAN1 panel labelled `CAN0←CAN1` so both panels list the buses in the same
  order
- Config and licences pages lay out correctly at phone width. The analyser
  console itself needs a laptop or PC

### DBC files
- No limit on the number of active files. The only limit is the total signal
  count held in RAM, raised from 768 to 2,000. Loading also stops early if free
  heap would drop below 64 KB
- **Signals** column on the DBC page: how many signals each file defines and,
  when a file only partly fits, how many were loaded (e.g. ⚠ 1,200 / 2,900).
  A banner says how many signals were dropped and why. Replaces the warnings
  based on file size
- Active files that no longer exist (e.g. after `uploadfs` or a filesystem OTA)
  are removed from the saved selection. Before, they kept counting as active, so
  the page could show "4 of 4 files" with one file installed
- The signal database (`/api/dbc/signals`) is streamed in chunks instead of
  built as one large string, so large DBC files no longer need ~100 KB of
  contiguous heap
- DBC loading runs only in the main loop, and the signal data is guarded by a
  lock. Before, the file list and signals requests could reload it while the
  message detail view was decoding from it
- DBC files load faster (buffered reads), and re-uploading an active file
  reloads it
- Selection errors (e.g. signal budget full) are shown on the page instead of
  "request interrupted"

### Fixes
- **DBC upload no longer reboots the unit.** The upload handler stored its
  result as a number cast to a pointer in `request->_tempObject`, which the web
  server later passes to `free()` — an assert and restart, dropping the WiFi AP
- No more `nvs_open failed: NOT_FOUND` on every boot: the TX scheduler and
  marker plan settings are created on first boot instead of only when first
  saved, and default config is saved on a fresh unit
- CustomOTA updated to 1.6.2: fixes `nvs_get_blob len fail: backup_ver
  NOT_FOUND` on every boot, and the backup's firmware version is shown again
  instead of "Previous Firmware (auto-detected)"

### WiFi
- Default device name is now `canbridge` (`http://canbridge.local`) instead of
  `canbridge-xxxxxx` from the MAC address. Units sharing a network still need
  their own names. Units that already saved a name keep it

### Documentation
- Printable in-box card (`manual/in-box-card.html`): WiFi details, wiring
  diagram, getting started, factory reset and open source notice
- Manual states that a laptop or PC is required, and explains how to find the
  unit at `http://<device name>.local` when it joins your WiFi

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
