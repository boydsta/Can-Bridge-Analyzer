# In-box Open Source Notice (pre-flashed boards)

Print this on the quick-start card or packing slip for every SPUD board
shipped with CAN Bridge Analyzer firmware installed. It provides the notice
and the 3-year written offer for source code required by GPL-3.0 §6(b),
LGPL-3.0 §4 and LGPL-2.1 §6(b).

Before printing, replace `<CONTACT>` with a real email or postal address.

---

**Open Source Software Notice**

This device ships with CAN Bridge Analyzer firmware (MIT License), which
includes third-party open source components: spud-diy (MIT), ACAN2517FD (MIT),
CustomOTA-ESP32 (MIT), ESP-IDF (Apache-2.0), Arduino core for ESP32
(LGPL-2.1), AsyncTCP (LGPL-3.0) and ESPAsyncWebServer (LGPL-3.0). Its
filesystem includes Nissan Leaf DBC data files (GPL-3.0).

Full copyright notices and licence texts are on the device at
**http://192.168.1.1/licenses** and at
**https://github.com/boydsta/Can-Bridge-Analyzer**.

**Written offer for source code:** the complete corresponding source code and
build instructions for the firmware on this unit are available at
https://github.com/boydsta/Can-Bridge-Analyzer/releases (the firmware version
and commit are shown on the /licenses page). In addition, for at least three
(3) years from the date you received this product, we will provide a complete
machine-readable copy of the corresponding source code for no more than the
cost of physically performing the distribution. To request a copy, contact
`<CONTACT>` and quote your firmware version.

You may modify and reinstall the firmware over USB or at
http://192.168.1.1/update.

---

## Release checklist for shipped firmware

1. Commit everything, so the /licenses page doesn't show a `-dirty` commit.
2. Tag the release (e.g. `v0.3.0`) and create a GitHub Release.
3. Attach a complete source archive that includes the PlatformIO library
   snapshots (`.pio/libdeps/<env>/`), so the source stays available for the
   3-year offer even if an upstream repo changes or disappears.
4. Flash boards with that tagged build and print this card.
