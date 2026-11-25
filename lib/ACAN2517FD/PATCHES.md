# Local patches to ACAN2517FD

This is a vendored copy of [ACAN2517FD](https://github.com/pierremolinaro/acan2517FD)
**v2.1.16** by Pierre Molinaro (MIT licence, see `LICENSE`), with the changes below.
The upstream `examples/` and `extras/` folders have been removed to keep the repo small.

**Do not replace this folder with a fresh upstream copy** without re-applying these
patches — the firmware depends on them for stable operation on a busy bus.

All changes are in `src/ACAN2517FD.cpp`.

## 1. Handler task stack 1024 → 2048 words (`begin`)

`xTaskCreate(myESP32Task, "ACAN2517Handler", 2048, ...)`

The 1024-word stack overflows on ESP32 under heavy CAN traffic. The task is
deliberately not pinned to CPU0 (the WiFi/BT stack runs there).

## 2. Per-frame interrupt masking in `isr_poll_core`

Upstream disables interrupts (`taskDISABLE_INTERRUPTS`) for the *entire* receive
drain loop. With many frames queued, each SPI frame read takes ~50–100 µs, so the
interrupts stay masked long enough to trigger an **interrupt watchdog (IWDT) panic**.
The disable/enable pair is moved inside the loop so interrupts are re-enabled
between iterations.

## 3. No interrupt masking in the register access functions

The `noInterrupts()` / `taskDISABLE_INTERRUPTS()` pairs around the SPI transactions in
`writeRegister32`, `writeRegister8`, `readRegister32`, `readRegister8` and
`readRegister16` are removed. They are redundant with the SPI transaction locking and
contributed to the same IWDT problem.

## 4. `recoverFromRestrictedOperationMode` timeout 10 ms → 1 ms

The mode-change wait is shortened so recovery attempts made from the main loop
don't stall it for up to 10 ms.
