# CAN Bridge Analyzer — How to Use

> **Draft** — firmware v0.3.0 + unreleased WiFi changes. Screenshots to follow.

This page assumes the unit is wired and you can open its web page. If it isn't
set up yet, start with [Hardware setup](setup.md).

## Contents

1. [The dashboard](#1-the-dashboard)
2. [Configuration](#2-configuration)
3. [Using several units](#3-using-several-units)
4. [DBC files (signal decoding)](#4-dbc-files-signal-decoding)
5. [TX Scheduler (sending frames)](#5-tx-scheduler-sending-frames)
6. [Test Markers](#6-test-markers)
7. [Logging to SavvyCAN over USB](#7-logging-to-savvycan-over-usb)
8. [Firmware updates](#8-firmware-updates)
9. [Troubleshooting](#9-troubleshooting)
10. [Reference](#10-reference)

---

## 1. The dashboard

Open **http://192.168.1.1** (own hotspot) or **http://&lt;device-name&gt;.local**
(when the unit has joined your network).

The page has three panels:

| Panel | Shows |
|-------|-------|
| **Left** | Frames received on CAN0 (forwarded to CAN1 when bridging) |
| **Centre** | Details of the message you selected |
| **Right** | Frames received on CAN1 (forwarded to CAN0 when bridging) |

### Message lists

Each CAN ID appears once in its panel, with its period and frame count. You can
sort the list by **ID**, **Period**, **Count** or **Activity**.

The coloured bar on the left of each row shows activity:

| Colour | Meaning |
|--------|---------|
| Green  | Data changed in the last 3 seconds |
| Blue   | Data is steady |
| Amber (flashes) | Something new happened: a new ID appeared, or a normally steady message just changed. The highlight lasts 15 s. |
| Grey   | The message has stopped arriving |

Amber is the colour to watch when you're reverse-engineering. Press a button,
open a door or turn a knob, and the message that reacts lights up.

### Message analysis (centre)

Click a row to see its ID, DLC, period and count, plus a byte view:

- **Curr / Prev**: the current and previous payload.
- **Diff**: which bytes changed between the two.
- **Decoded Signals**: signal names and values, when a loaded DBC file
  describes this ID (see [section 4](#4-dbc-files-signal-decoding)).

### Pin and Block

- **📌 Pin** keeps the selected message at the top of its panel.
- **🚫 Block** stops the selected ID from being forwarded to the other bus. The
  message is still shown; it just isn't bridged. Blocking only matters when both
  buses are enabled. **Blocks are cleared when the unit restarts.**

### Header bar

The header shows the WebSocket connection, whether CAN data is arriving, the
total message count, and the **⬤ Serial Log** toggle. It also links to the other
pages: TX Sched, Markers and Config (far right).

Each bus panel heading has a status badge (hover over it for the error
counters):

| Badge | Meaning |
|-------|---------|
| 🟢 **Live** | Frames received in the last 3 s |
| 🟠 **Live · errors** | Receiving, but with errors. Check termination and speed |
| 🟠 **No ACK** | Frames sent to this bus aren't acknowledged. Usually nothing is connected |
| 🟠 **RX errors** | Corrupted frames. Usually the wrong bus speed |
| ⚪ **No traffic** | Nothing received. Not connected, or the bus is quiet |
| 🔴 **Bus off** / **No controller** | Controller stopped, or failed to start |

On its own, an unplugged bus looks the same as a quiet one (**No traffic**).
It shows **No ACK** once the bridge or TX Scheduler tries to send to it.

The Theme toggle, DBC files and firmware updates are on the Config page.

---

## 2. Configuration

Open **⚙ Config** from the dashboard.

The page has four cards, top to bottom:

| Card | Applies |
|------|---------|
| **Serial logging**: output format | Straight away, when you press **Apply** |
| **Bus speed** and **WiFi** | When you press **Save & Reboot**. The unit restarts |
| **DBC files**: shows the active files, with **Manage DBC files** | See [section 4](#4-dbc-files-signal-decoding) |
| **Firmware**: version, with **Update firmware** | See [section 8](#8-firmware-updates) |

### WiFi

The top of the card shows the **current status**: which network the unit is on
and its address.

Choose one of two modes:

- **Own hotspot** (default). The unit creates its own WiFi network at
  http://192.168.1.1. This is simplest for a single unit.
- **Join a network**. The unit joins your router, workshop WiFi or phone
  hotspot. Use this when you have more than one unit
  (see [section 3](#3-using-several-units)).

When **Join a network** is selected, these settings appear:

| Setting | Notes |
|---------|-------|
| Network to join (SSID) | The name of your router or hotspot. 2.4 GHz only. The unit can't see 5 GHz networks. |
| Network password | Leave blank for an open network. |
| Device name | The name you use to reach the unit: `http://<name>.local`. Also shown in your router's device list. Letters, digits and `-` only. Leave blank to use the default, `canbridge`. Give each unit its own name if several share a network. |
| Automatic IP (DHCP) | Recommended. The router picks the address; use the device name to find the unit. |
| Static IP | You choose the IP address, subnet mask, gateway and (optionally) DNS. The address must be in your network's range but outside the router's DHCP pool. |

> **Phone hotspots:** use DHCP. Android changes its subnet randomly, and iPhone
> uses 172.20.10.x, so a static IP set for one won't work on another.

**Own / fallback hotspot.** These are the name and password of the unit's own
network. That network is used in **Own hotspot** mode, and also as the
**fallback**: if the unit can't join your network within 20 seconds, or loses
it for 20 seconds, it starts this hotspot at http://192.168.1.1 so you can
still reach it. While the hotspot is running, the unit tries your network again
every minute, but only when nobody is connected to the hotspot.

After you save in **Join a network** mode, the confirmation page shows the
address to use. Connect your computer to the same network and open that
address.

#### Finding the unit on your network

1. Connect your laptop to the **same WiFi network** the unit joined.
2. In the browser, open **http://canbridge.local**. If you changed the
   **Device name** in ⚙ Config, use that instead: `http://<device name>.local`.
   Names are always lowercase, and you need to type the `http://` so the
   browser doesn't run a search instead.

   > **Several units on one network** must each have a different device name
   > (see [section 3](#3-using-several-units)). Two units both called
   > `canbridge` clash, and only one of them can be reached.

If the `.local` address doesn't open:

- **Check the unit joined.** If your WiFi list shows the unit's own hotspot
  (`CanBridgeAnalyzer` by default), it couldn't join your network. Connect to
  the hotspot, open http://192.168.1.1, and check the network name and
  password. The network must be 2.4 GHz.
- **Use the IP address.** Find the unit by its device name in your router's
  list of connected devices, and open `http://<IP address>`. To keep that
  address from changing, reserve it in the router or set a **Static IP**.
- **Some systems don't support `.local` names**, for example some Android
  devices, corporate networks, and Windows laptops on a VPN. Use the IP address
  instead.

If you lock yourself out, use the
[factory reset](setup.md#7-factory-reset).

### Bus speed and frame type

One preset applies to both buses:

| Preset | Use for |
|--------|---------|
| 125 / 250 / 500 kbps, 1 Mbps — CAN 2.0 | Classic CAN. Most vehicles use 500 kbps. |
| 500k/2M, 500k/4M, 1M/4M, 1M/8M — CAN FD | CAN FD buses (arbitration / data rate). |

Every node on a bus must use the same speed and frame type. A mismatch causes
errors and can stop the bus. CAN FD presets are only offered on the MCP251863
(SPUD) build.

### CAN0 and CAN1

Both buses are always on. The unit monitors both and forwards frames between
them (bridge). There is nothing to set: a bus with nothing wired to it simply
receives nothing.

If only one bus is wired, frames bridged towards the empty side get no
acknowledgement. In **CAN Diagnostics** mode that bus then reports NACK and
Error Passive. This is expected and does no harm.

---

## 3. Using several units

When each unit runs its own hotspot, your laptop can only be on one of them at
a time. To reach several units together:

1. Connect to each unit in turn on its own hotspot and open **⚙ Config**.
2. Select **Join a network** and enter your router's (or phone hotspot's)
   name and password.
3. Give each unit a different **device name**, for example `canbridge-front`,
   `canbridge-rear` and `canbridge-bench`.
4. Also give each unit a different **hotspot name** (for example
   `CanBridge-Front`). Then, if one falls back to its hotspot, you can tell
   which unit it is.
5. **Save & Reboot.**
6. Connect your laptop to the same network and open
   `http://canbridge-front.local`, `http://canbridge-rear.local`, and so on,
   each in its own browser tab.

If a `.local` name doesn't open (some Android devices and corporate networks
don't support it), use the IP address from your router's device list, or set
static IPs.

### Several people or browsers on one unit

Connect **no more than two devices** (phones, tablets or laptops) to a unit at
the same time. Both see the same live data. More devices slow the dashboard
down and can make it disconnect.

Settings are shared: Block, TX Scheduler, Markers, DBC and Config changes made
on one device apply to both.

---

## 4. DBC files (signal decoding)

A DBC file describes which signals are carried in which CAN IDs. Once a file is
loaded, the dashboard shows signal names and values next to the raw bytes.

Open **⚙ Config**, then **📄 Manage DBC files** (or go to `/dbc`):

- **Upload Over WiFi**: click **Choose .dbc file**, then **Upload**.
- **Installed Files**: tick a file to make it active. Any number of files can
  be active, and their signals are merged. Your selection is kept after a
  restart. The **Signals** column shows how many signals each file defines.
- Signals are held in RAM, so the limit is the total number of signals (2,000),
  not the number of files. If a file doesn't fully fit, its row shows how many
  signals were loaded (for example ⚠ 1,200 / 2,900) and a warning appears;
  messages whose signals were dropped show no decode. Untick a file to free space.
- Files that were active but no longer exist (for example after re-uploading the
  filesystem image) are removed from the selection automatically.

Nissan Leaf DBC files (EV-CAN, CAR-CAN, QC-CAN) are included if the filesystem
was loaded at install time.

---

## 5. TX Scheduler (sending frames)

> ⚠ **Sending frames onto a vehicle bus can affect how the vehicle behaves.**
> Only send frames you understand, and test on the bench first.

Open **🔁 TX Sched** to transmit frames repeatedly:

| Field | Meaning |
|-------|---------|
| CAN ID (hex) | e.g. `50C` |
| Bus | CAN0 or CAN1 |
| Interval (ms) | How often to send |
| Counter byte | `-1` sends the same payload every time. `0`–`7` makes that byte count up by one on every send (for rolling-counter keep-alives). |
| Data (hex) | 8 bytes, e.g. `0000000000000000` |
| Label | Your own note |

New entries start **disabled**. Enable each one once you've checked the wiring
and the frame contents. Entries are saved and survive a restart.

---

## 6. Test Markers

Markers record *when* you did something, inside the same log you're capturing.
That way you don't need a separate notebook of timestamps.

1. Open **📌 Markers** and tap **✎ Plan**.
2. Write one step per line (e.g. `Neutral`, `Engage 1st gear`,
   `Rev to 3000 rpm`), then tap **Save Plan**.
3. Start your SavvyCAN log (see [section 7](#7-logging-to-savvycan-over-usb)).
4. Do each step on the vehicle, then immediately tap **MARK**.
5. Tap **↻ Reset** to begin a new run. The run number goes up by one.

Each tap adds a frame with ID **0x7FE** to the log:

| Byte | Value |
|------|-------|
| 0 | Step number (0 = first) |
| 1 | Run number |
| 2–5 | Device time in ms (little-endian) |
| 6 | Total steps in the plan |
| 7 | `0xBE` (marker signature) |

In SavvyCAN, filter on ID `0x7FE` to jump between your markers. Several phones
or tabs can have the page open at once and they all share the same step, so
one person can drive while another taps.

---

## 7. Logging to SavvyCAN over USB

Connect the SPUD's USB port to your computer and use **115200 baud**. Choose the
format under **⚙ Config → Serial logging**. It applies straight away:

| Mode | Use |
|------|-----|
| **CAN Diagnostics** | No frame log. A bus health report every few seconds: error counters, bus state, and wiring/termination advice. Use this when commissioning. |
| **SavvyCAN CSV** | Save the serial output to a `.csv` file, then in SavvyCAN use *File → Load Log File → GVRET Logs*. |
| **Binary GVRET** | Live capture in SavvyCAN: *Add New Device Connection → GVRET Serial* and pick the COM port. If the connection drops, turn off *Validate Communications* in SavvyCAN's settings. |

CSV and GVRET log both buses; the `Bus` column (0 or 1) tells them apart.
Frames sent by the [TX Scheduler](#5-tx-scheduler-sending-frames) are logged
too, on the bus they went out on, and marked `Tx` in the CSV `Dir` column.
[Test markers](#6-test-markers) are always logged.

The **⬤ Serial Log** button on the dashboard starts and stops serial output
without restarting the unit. Hover over it to see the current format. It is
always off after a reboot. In **Binary GVRET** mode it also
switches on by itself when SavvyCAN connects.

---

## 8. Firmware updates

1. Open **⚙ Config**, then **⬆ Update firmware** (or go to `/update`).
2. Upload `firmware.bin` (the firmware) or `spiffs.bin` (the DBC file
   filesystem).
3. The unit restarts on the new version. The previous firmware is kept; use
   **Rollback** to go back to it.

- **Updates are blocked while CAN traffic is flowing**, because the restart
  would interrupt the bridge. Switch the vehicle off or unplug CAN. The page
  unlocks once both buses have been quiet for 3 seconds.
- Uploading `spiffs.bin` **replaces all DBC files**, including ones you
  uploaded yourself.

---

## 9. Troubleshooting

| Symptom | Check |
|---------|-------|
| Can't see the `CanBridgeAnalyzer` network | Allow 10 s after power-on. If the unit is set to **Join a network**, its hotspot only appears after it fails to join (about 20 s). If you're stuck, do a [factory reset](setup.md#7-factory-reset). |
| `http://<name>.local` doesn't open | Make sure your computer is on the same network as the unit. Some Android devices don't support `.local`; use the IP from your router's device list instead. |
| The unit keeps falling back to its hotspot | Check the network name and password (case-sensitive), make sure it's a 2.4 GHz network, and check that the unit is in range. A wrong static IP can also stop it working. |
| No messages appear | Check the bus speed matches the vehicle, the bus is enabled in Config, and CAN-H/CAN-L aren't swapped. Switch serial to **CAN Diagnostics** and read the report. |
| Errors or bus-off | Speed or frame-type mismatch, or wrong [termination](setup.md#3-termination). |
| Dashboard is slow or keeps disconnecting | Too many devices are connected. Keep it to two (see [section 3](#several-people-or-browsers-on-one-unit)). |
| OTA page is greyed out | CAN traffic is flowing. Switch the vehicle off or unplug CAN. |
| SavvyCAN won't connect | Use **Binary GVRET** mode, the right COM port and 115200 baud, and turn off *Validate Communications* in SavvyCAN. |

---

## 10. Reference

### Factory defaults

| Setting | Default |
|---------|---------|
| WiFi mode | Own hotspot |
| Hotspot name / password | `CanBridgeAnalyzer` / `canbus123` |
| Hotspot address | http://192.168.1.1 |
| Device name | `canbridge` |
| Bus preset | 500 kbps CAN 2.0 |
| Serial mode | SavvyCAN CSV |

### Web pages

| Address | Page |
|---------|------|
| `/` | Dashboard |
| `/config` | Configuration |
| `/dbc` | DBC files |
| `/tx` | TX Scheduler |
| `/markers` | Test Markers |
| `/update` | Firmware update |
