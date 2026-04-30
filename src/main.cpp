/** @file main.cpp
    @brief CAN Bridge Analyzer — main entry point
    @details CAN bus capture, analysis, and bridging tool for the SPUD board.
             Supports CAN 2.0 and CAN FD via MCP2517FD/MCP251863 controllers.
             Configuration is stored in NVS and managed via the WiFi web interface.
*/

#include <Arduino.h>
#include <Preferences.h>

#define MCP251863 2
#define CAN_CHIP MCP251863

#include <ACAN2517FD.h>

#define SPUD_VERSION 1
#include "spud.h"
#include "can_bridge.h"
#include "web_server.h"
#include "can_config.h"

#define ARB_BIT_RATE 500000  // fallback for can_init default arg only

/* Both controllers always compiled — enabled/disabled at runtime via g_cfg. */
ACAN2517FD can0(CS0, SPI, INT0);
ACAN2517FD can1(CS1, SPI, INT1);

CANBridge* can_bridge = nullptr;
CANWebServer* web_server = nullptr;

bool can0_success = false;
bool can1_success = false;

CANFDMessage frame;
const uint8_t max_messages_per_call = 64;
uint8_t messages_processed = 0;
unsigned long can1MessageProcTime = 0;
unsigned long can0MessageProcTime = 0;

/* Active preset index — set in setup() from g_cfg.bus_preset. */
uint8_t g_bus_preset_idx = k_default_preset;

/** @brief Full runtime configuration — loaded from NVS in setup(). */
CANConfig g_cfg;

/** @brief Runtime serial logging flag.
 *  true  = CAN serial log to USB — suppress debug prints.
 *  false = debug prints enabled (default at boot).
 *  Toggled in real time via the web UI button. */
volatile bool g_serial_logging = false;

/** @brief Serial log format selector.
 *  true  = GVRET / SavvyCAN format (default).
 *  false = Kvaser .asc format. */
volatile bool g_serial_log_gvret = true;

/** @brief Serial output mode.
 *  0 = CAN Diagnostics  — debug prints + periodic bus health report.
 *  1 = SavvyCAN CSV     — text CSV, save to file then import in SavvyCAN.
 *  2 = Binary GVRET     — binary protocol, connect SavvyCAN live via serial.
 *  Loaded from NVS at boot; updated by /api/set_config. */
volatile uint8_t g_serial_mode = 1;

// Performance monitoring
struct PerformanceStats {
  unsigned long main_loop_min = ULONG_MAX;
  unsigned long main_loop_max = 0;
  unsigned long handle_can_min = ULONG_MAX;
  unsigned long handle_can_max = 0;
  unsigned long websocket_min = ULONG_MAX;
  unsigned long websocket_max = 0;
  unsigned long can0_proc_time_min = ULONG_MAX;
  unsigned long can0_proc_time_max = 0;
  unsigned long can1_proc_time_min = ULONG_MAX;
  unsigned long can1_proc_time_max = 0;
  unsigned long can0_n_proc_max = 0;
  unsigned long can1_n_proc_max = 0;
  unsigned long last_stats_print = 0;

  void reset() {
    main_loop_min = ULONG_MAX;
    main_loop_max = 0;
    handle_can_min = ULONG_MAX;
    handle_can_max = 0;
    websocket_min = ULONG_MAX;
    websocket_max = 0;
    can0_proc_time_min = ULONG_MAX;
    can0_proc_time_max = 0;
    can1_proc_time_min = ULONG_MAX;
    can1_proc_time_max = 0;
    can0_n_proc_max = 0;
    can1_n_proc_max = 0;
  }

  void update_main_loop(unsigned long duration) {
    if (duration < main_loop_min) main_loop_min = duration;
    if (duration > main_loop_max) main_loop_max = duration;
  }

  void update_handle_can(unsigned long duration) {
    if (duration < handle_can_min) handle_can_min = duration;
    if (duration > handle_can_max) handle_can_max = duration;
  }

  void update_can0_proc(unsigned long duration, unsigned int n_proc) {
    if (duration < can0_proc_time_min) can0_proc_time_min = duration;
    if (duration > can0_proc_time_max) can0_proc_time_max = duration;
    if (n_proc > can0_n_proc_max) can0_n_proc_max = n_proc;
  }

  void update_can1_proc(unsigned long duration, unsigned int n_proc) {
    if (duration < can1_proc_time_min) can1_proc_time_min = duration;
    if (duration > can1_proc_time_max) can1_proc_time_max = duration;
    if (n_proc > can1_n_proc_max) can1_n_proc_max = n_proc;
  }

  void update_websocket(unsigned long duration) {
    if (duration < websocket_min) websocket_min = duration;
    if (duration > websocket_max) websocket_max = duration;
  }

  void print_stats() {
    Serial.printf("Loop: min=%luµs max=%luµs  CAN: min=%luµs max=%luµs  WS: min=%luµs max=%luµs\n",
                  main_loop_min == ULONG_MAX ? 0 : main_loop_min, main_loop_max,
                  handle_can_min == ULONG_MAX ? 0 : handle_can_min, handle_can_max,
                  websocket_min  == ULONG_MAX ? 0 : websocket_min,  websocket_max);
  }
};

PerformanceStats perf_stats;

/** @brief IRAM_ATTR ISR wrappers for CAN controllers.
 *  Lambdas are NOT placed in IRAM — during SPI flash cache arbitration the
 *  CPU cannot fetch lambda code, causing missed INT edges and IWDT resets.
 *  These named functions live in IRAM and are never evicted. */
void IRAM_ATTR can0_isr_wrapper() { can0.isr(); }
void IRAM_ATTR can1_isr_wrapper() { can1.isr(); }

/** @brief Drain pending CAN INT events at the top of loop().
 *  MCP251863 INT is active-LOW level-triggered. A second event arriving
 *  while the ISR is executing keeps the pin asserted but won't re-trigger
 *  the ISR. Polling here (bounded to 16 iterations) ensures no frames are
 *  stranded in the controller FIFO. */
static inline void handle_deferred_can_isr() {
  if (g_cfg.can0_enable) {
    for (int i = 0; i < 16 && digitalRead(INT0) == LOW; i++) { can0.isr(); }
  }
  if (g_cfg.can1_enable) {
    for (int i = 0; i < 16 && digitalRead(INT1) == LOW; i++) { can1.isr(); }
  }
}

/** @brief Flush stale SPI transaction left in MCP251863 after an ESP32 reset.
 *  GPIO pins float briefly during the ESP32 reset edge, which can leave the
 *  MCP SPI state machine mid-transaction. Clocking 16 x 0xFF bytes discards
 *  the partial transaction, then 0xC0 forces a clean hardware controller reset. */
static void mcp_spi_flush(uint8_t cs_pin) {
  digitalWrite(cs_pin, LOW);
  delayMicroseconds(10);
  for (int i = 0; i < 16; i++) SPI.transfer(0xFF);
  delayMicroseconds(10);
  digitalWrite(cs_pin, HIGH);
  delayMicroseconds(100);
  digitalWrite(cs_pin, LOW);
  delayMicroseconds(10);
  SPI.transfer(0xC0);
  delayMicroseconds(10);
  digitalWrite(cs_pin, HIGH);
  vTaskDelay(pdMS_TO_TICKS(5));
}

/**
 * @brief Initialize CAN controllers.
 * @return true if at least one controller initialised successfully.
 */
static bool can_init() {
  delay(500);

  /* ACAN2517FD manages CS internally via digitalWrite — do not pass a CS pin
     to SPI.begin() or the ESP32 SPI peripheral will fight with the library. */
  SPI.begin(SCLK, SDI, SDO);
  delay(500);

  if (g_cfg.can0_enable) {
    mcp_spi_flush(CS0);
    const BusPreset& p = k_bus_presets[g_cfg.bus_preset];
    ACAN2517FDSettings settings0(ACAN2517FDSettings::OSC_40MHz, p.arb_bps,
                                 (DataBitRateFactor)p.data_mult);
    settings0.mRequestedMode = p.is_fd ? ACAN2517FDSettings::NormalFD : ACAN2517FDSettings::Normal20B;
    settings0.mControllerTransmitFIFOSize = 12;
    settings0.mControllerReceiveFIFOSize = 12;
    /* ThreeAttempts prevents INT staying LOW continuously on a quiet bus.
       UnlimitedNumber (default) fires TX-Attempt-Exhausted interrupt on every
       retry, holding INT LOW and driving ACAN2517Handler into a tight ISR loop
       which causes an IWDT watchdog reset. */
    settings0.mControllerTransmitFIFORetransmissionAttempts = ACAN2517FDSettings::ThreeAttempts;
    uint32_t errorCode0 = 0;
    int can0_attempts = 0;
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Attempting CAN0 init..."); }
    while ((errorCode0 = can0.begin(settings0, can0_isr_wrapper)) != 0 && can0_attempts < 5) {
      if (g_serial_logging && g_serial_mode == 0) {
        Serial.print("CAN bus 0 init failed, error code: ");
        Serial.println(errorCode0);
        Serial.println(" Trying again...");
      }
      can0_attempts++;
      delay(500);
    }

    if (errorCode0 == 0) {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("-------------CAN0 SUCCESS-------------"); }
      can0_success = true;
    } else {
      if (g_serial_logging && g_serial_mode == 0) {
        Serial.println("-------------CAN0 FAILED - CONTINUING WITHOUT CAN-------------");
        Serial.printf("Final error code: %u after %d attempts\n", errorCode0, can0_attempts);
      }
      can0_success = false;
    }
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.print("Bit Rate prescaler: ");
      Serial.println(settings0.mBitRatePrescaler);
      Serial.print("Arbitration Phase segment 1: ");
      Serial.println(settings0.mArbitrationPhaseSegment1);
      Serial.print("Arbitration Phase segment 2: ");
      Serial.println(settings0.mArbitrationPhaseSegment2);
      Serial.print("Arbitration SJW:");
      Serial.println(settings0.mArbitrationSJW);
      Serial.print("Actual Arbitration Bit Rate: ");
      Serial.print(settings0.actualArbitrationBitRate());
      Serial.println(" bit/s");
      Serial.print("Exact Arbitration Bit Rate ? ");
      Serial.println(settings0.exactArbitrationBitRate() ? "yes" : "no");
      Serial.print("Arbitration Sample point: ");
      Serial.print(settings0.arbitrationSamplePointFromBitStart());
      Serial.println("%");
      Serial.print("Data Phase segment 1: ");
      Serial.println(settings0.mDataPhaseSegment1);
      Serial.print("Data Phase segment 2: ");
      Serial.println(settings0.mDataPhaseSegment2);
      Serial.print("Data SJW:");
      Serial.println(settings0.mDataSJW);
      Serial.print("TDCO:");
      Serial.println(settings0.mTDCO);
      if (errorCode0 == 0) { Serial.println("Successful setup of CAN0"); }
    }
  } else {
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Skipping CAN0 setup (not enabled)."); }
    can0_success = true;
  }

  if (g_cfg.can1_enable) {
    mcp_spi_flush(CS1);
    const BusPreset& p1 = k_bus_presets[g_cfg.bus_preset];
    ACAN2517FDSettings settings1(ACAN2517FDSettings::OSC_40MHz, p1.arb_bps,
                                 (DataBitRateFactor)p1.data_mult);
    settings1.mRequestedMode = p1.is_fd ? ACAN2517FDSettings::NormalFD : ACAN2517FDSettings::Normal20B;
    settings1.mControllerTransmitFIFOSize = 12;
    settings1.mControllerReceiveFIFOSize = 12;
    settings1.mControllerTransmitFIFORetransmissionAttempts = ACAN2517FDSettings::ThreeAttempts;
    uint32_t errorCode1 = 0;
    int can1_attempts = 0;
    const int max_can1_attempts = 5;

    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Attempting CAN1 init..."); }
    errorCode1 = can1.begin(settings1, can1_isr_wrapper);

    while ((errorCode1 != 0) && (can1_attempts < max_can1_attempts)) {
      can1_attempts++;
      if (g_serial_logging && g_serial_mode == 0) {
        Serial.printf("CAN bus 1 init failed, error code: %u\n", errorCode1);
        Serial.printf(" Trying again... (attempt %d/%d)\n", can1_attempts, max_can1_attempts);
      }
      delay(1000);
      errorCode1 = can1.begin(settings1, can1_isr_wrapper);
    }
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.println("-------------CAN1-------------");
      Serial.print("Bit Rate prescaler: ");
      Serial.println(settings1.mBitRatePrescaler);
      Serial.print("Arbitration Phase segment 1: ");
      Serial.println(settings1.mArbitrationPhaseSegment1);
      Serial.print("Arbitration Phase segment 2: ");
      Serial.println(settings1.mArbitrationPhaseSegment2);
      Serial.print("Arbitration SJW:");
      Serial.println(settings1.mArbitrationSJW);
      Serial.print("Actual Arbitration Bit Rate: ");
      Serial.print(settings1.actualArbitrationBitRate());
      Serial.println(" bit/s");
      Serial.print("Exact Arbitration Bit Rate ? ");
      Serial.println(settings1.exactArbitrationBitRate() ? "yes" : "no");
      Serial.print("Arbitration Sample point: ");
      Serial.print(settings1.arbitrationSamplePointFromBitStart());
      Serial.println("%");
      Serial.print("Data Phase segment 1: ");
      Serial.println(settings1.mDataPhaseSegment1);
      Serial.print("Data Phase segment 2: ");
      Serial.println(settings1.mDataPhaseSegment2);
      Serial.print("Data SJW:");
      Serial.println(settings1.mDataSJW);
      Serial.print("TDCO:");
      Serial.println(settings1.mTDCO);
    }
    if (errorCode1 == 0) {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("-------------CAN1 SUCCESS-------------"); }
      can1_success = true;
    } else {
      if (g_serial_logging && g_serial_mode == 0) {
        Serial.println("-------------CAN1 FAILED - CONTINUING WITHOUT CAN-------------");
        Serial.printf("Final error code: %u after %d attempts\n", errorCode1, can1_attempts);
      }
      can1_success = false;
    }
  } else {
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Skipping CAN1 setup (not enabled)."); }
    can1_success = true;
  }

  return (can0_success || can1_success);
}

/**
 * @brief Drain RX buffers on both CAN buses before bridge initialization.
 * @details Reads and discards messages that arrived during CAN init.
 */
void drain_can_rx_buffers() {
  const uint8_t max_drain_per_call = 100;
  uint8_t total_drained = 0;

  if (g_cfg.can0_enable) {
    if (can0_success) {
      uint8_t can0_drained = 0;
      while (can0.receive(frame) && can0_drained < max_drain_per_call) {
        can0_drained++;
      }
      if (can0_drained > 0) {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Drained %d messages from CAN0 RX buffer\n", can0_drained); }
        total_drained += can0_drained;
      } else {
        if (g_serial_logging && g_serial_mode == 0) { Serial.println("Drained 0 messages from CAN0"); }
      }
    } else {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("CAN0 not active"); }
    }
  }

  if (g_cfg.can1_enable) {
    if (can1_success) {
      uint8_t can1_drained = 0;
      while (can1.receive(frame) && can1_drained < max_drain_per_call) {
        can1_drained++;
      }
      if (can1_drained > 0) {
        if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Drained %d messages from CAN1 RX buffer\n", can1_drained); }
        total_drained += can1_drained;
      } else {
        if (g_serial_logging && g_serial_mode == 0) { Serial.println("Drained 0 messages from CAN1"); }
      }
    } else {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("CAN1 not active"); }
    }
  }

  if (total_drained > 0) {
    if (g_serial_logging && g_serial_mode == 0) { Serial.printf("Total drained before bridge init: %d messages\n", total_drained); }
  } else {
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("No messages to drain before bridge initialization"); }
  }
}

/**
 * @brief Print CAN frame to serial in the active logging format.
 * @param bus  CAN bus index (0 or 1)
 * @param id   CAN frame ID
 * @param len  Data length
 * @param buf  Data bytes
 */
void print_can_mesg(unsigned short bus, long unsigned int id, unsigned short len, uint8_t *buf) {
  if (g_serial_mode == 2) {
    /* Binary GVRET frame — SavvyCAN live serial connection (Add New Device → GVRET Serial).
       Format: [0xF1][0x00][ts:4LE µs][id:4LE, bit31=ext][dlc|(bus<<4)][data...] */
    uint32_t tNow = (uint32_t)(millis() * 1000UL);
    uint32_t gvret_id = (uint32_t)id;
    if (id > 0x7FF) gvret_id |= (1u << 31);
    uint8_t dlc = (len > 8) ? 8 : (uint8_t)len;
    uint8_t frame[20];
    frame[0]  = 0xF1;
    frame[1]  = 0x00;
    frame[2]  = (uint8_t)(tNow & 0xFF);
    frame[3]  = (uint8_t)((tNow >> 8) & 0xFF);
    frame[4]  = (uint8_t)((tNow >> 16) & 0xFF);
    frame[5]  = (uint8_t)((tNow >> 24) & 0xFF);
    frame[6]  = (uint8_t)(gvret_id & 0xFF);
    frame[7]  = (uint8_t)((gvret_id >> 8) & 0xFF);
    frame[8]  = (uint8_t)((gvret_id >> 16) & 0xFF);
    frame[9]  = (uint8_t)((gvret_id >> 24) & 0xFF);
    frame[10] = (uint8_t)(dlc | ((bus & 0x0F) << 4));
    for (uint8_t i = 0; i < dlc; i++) frame[11 + i] = buf[i];
    Serial.write(frame, 11 + dlc);
    return;
  }
  /* Mode 1: SavvyCAN native CSV — save to .csv, import via File → Load Log File → GVRET Logs.
     Header:  Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
     Timestamp in microseconds; ID uppercase zero-padded 8-digit hex;
     Extended "true"/"false"; always 8 data columns padded with 00; trailing comma. */
  unsigned long tNow = millis() * 1000UL;
  const char* ext = (id > 0x7FF) ? "true" : "false";
  Serial.printf("%lu,%08lX,%s,Rx,%u,%u", tNow, id, ext, bus, len);
  for (int i = 0; i < 8; i++) {
    Serial.printf(",%02X", (i < len) ? buf[i] : 0);
  }
  Serial.print(",\n");
}

/**
 * @brief Print CAN bus health diagnostics to serial.
 * @details Called every 5 s when g_serial_mode == 0.  Reads hardware error
 *          counters (TEC/REC) and BDIAG registers from each enabled CAN
 *          controller and prints a human-readable status report.
 */
void print_can_diagnostics() {
  unsigned long upSec = millis() / 1000;
  Serial.printf("\n=== CAN Diagnostics  uptime %lus ===\n", upSec);

  auto report_bus = [](const char* label, ACAN2517FD& ctrl, bool fd_mode, uint32_t bps) {
    uint32_t ec = ctrl.errorCounters();
    uint8_t  rec   = (uint8_t)(ec & 0xFF);
    uint8_t  tec   = (uint8_t)((ec >> 8) & 0xFF);
    bool     ewarn = (ec >> 16) & 1;
    bool     rxwarn= (ec >> 17) & 1;
    bool     txwarn= (ec >> 18) & 1;
    bool     rxbp  = (ec >> 19) & 1;
    bool     txbp  = (ec >> 20) & 1;
    bool     txbo  = (ec >> 21) & 1;

    const char* state;
    if (txbo)              state = "BUS OFF";
    else if (txbp || rxbp) state = "Error Passive";
    else if (ewarn)        state = "Error Warning";
    else                   state = "Active — OK";

    Serial.printf("[%s] %s  REC=%u  TEC=%u  %s  %ubps\n",
                  label, state, rec, tec, fd_mode ? "CAN FD" : "CAN 2.0", bps);

    if (txbo) {
      Serial.printf("[%s] !! BUS OFF — controller has stopped. TEC reached 256.\n", label);
      Serial.printf("[%s]    Likely causes (fix the highest one first):\n", label);
      Serial.printf("[%s]    1. Baud rate mismatch — this node set to %ubps, check all others match\n", label, bps);
      Serial.printf("[%s]    2. Missing termination — need 120 ohm at EACH physical end of cable\n", label);
      Serial.printf("[%s]    3. CANH/CANL wires swapped or one wire open/short\n", label);
      Serial.printf("[%s]    4. CAN FD/2.0 mismatch — every node must use the same frame type\n", label);
      Serial.printf("[%s]    Reboot after fixing to exit bus-off state.\n", label);
    } else if (txbp || rxbp) {
      Serial.printf("[%s] !! Error Passive — persistent errors, counter > 127.\n", label);
      if (txbp) Serial.printf("[%s]    High TEC (%u): this node can't get its frames through.\n", label, tec);
      if (rxbp) Serial.printf("[%s]    High REC (%u): this node is seeing corrupted incoming frames.\n", label, rec);
      Serial.printf("[%s]    Likely causes:\n", label);
      Serial.printf("[%s]    1. Baud rate mismatch — verify all nodes are at %ubps\n", label, bps);
      Serial.printf("[%s]    2. Termination — measure CANH-CANL with bus idle: should be ~2.5V.\n", label);
      Serial.printf("[%s]       Power off and measure resistance end-to-end: should be ~60 ohm\n", label);
      Serial.printf("[%s]       (two 120 ohm resistors in parallel, one at each cable end).\n", label);
      Serial.printf("[%s]    3. Cable too long for this baud rate — at %ubps keep under %um\n",
                    label, bps, bps >= 1000000 ? 5 : bps >= 500000 ? 10 : bps >= 250000 ? 25 : 100);
      Serial.printf("[%s]    4. CAN FD/2.0 mismatch — all nodes must agree on frame type\n", label);
    } else if (ewarn) {
      Serial.printf("[%s] ! Error Warning — occasional errors, counter 96-127.\n", label);
      if (txwarn) {
        Serial.printf("[%s]   High TEC (%u): TX errors. Likely termination issue or no other\n", label, tec);
        Serial.printf("[%s]   powered node on the bus to acknowledge frames.\n", label);
      }
      if (rxwarn) {
        Serial.printf("[%s]   High REC (%u): RX errors. Likely cable noise or baud mismatch\n", label, rec);
        Serial.printf("[%s]   from another node on the bus.\n", label);
      }
    } else if (rec == 0 && tec == 0) {
      Serial.printf("[%s]   Bus healthy. No errors.\n", label);
    } else {
      Serial.printf("[%s]   Low error counts (REC=%u TEC=%u) — bus is healthy.\n", label, rec, tec);
    }

    uint32_t bd1 = ctrl.diagInfos(1);
    static const uint32_t ERR_BITS = (1u<<22)|(1u<<20)|(1u<<19)|(1u<<18)|(1u<<17)|(1u<<16);
    if (bd1 & ERR_BITS) {
      Serial.printf("[%s] Active error flags:\n", label);
      if (bd1 & (1u << 22)) {
        Serial.printf("[%s]   NACK — no other node acknowledged the last transmitted frame.\n", label);
        Serial.printf("[%s]          Is there another powered CAN node on the bus?\n", label);
        Serial.printf("[%s]          Check termination (120 ohm at each cable end).\n", label);
        Serial.printf("[%s]          Verify baud rate matches on all nodes (%ubps).\n", label, bps);
      }
      if (bd1 & (1u << 20)) {
        Serial.printf("[%s]   CRC-ERR — frame checksum failed; data was corrupted in transit.\n", label);
        Serial.printf("[%s]          Causes: electrical noise, marginal termination, long cable,\n", label);
        Serial.printf("[%s]          or baud rate slightly off (crystal/oscillator mismatch).\n", label);
      }
      if (bd1 & (1u << 19)) {
        Serial.printf("[%s]   STUFF-ERR — bit stuffing rule violated.\n", label);
        Serial.printf("[%s]          Most common cause: baud rate mismatch between nodes.\n", label);
        Serial.printf("[%s]          Also caused by severe electrical noise or bus reflections\n", label);
        Serial.printf("[%s]          (missing termination on a long cable).\n", label);
      }
      if (bd1 & (1u << 18)) {
        Serial.printf("[%s]   FORM-ERR — frame structure is wrong (e.g. bad EOF bits).\n", label);
        Serial.printf("[%s]          Likely cause: baud rate mismatch, or CAN FD frame seen\n", label);
        Serial.printf("[%s]          by a CAN 2.0-only node (or vice versa).\n", label);
      }
      if (bd1 & (1u << 17)) {
        Serial.printf("[%s]   BIT1-ERR — node sent recessive (1) but bus was dominant (0).\n", label);
        Serial.printf("[%s]          During arbitration this is normal; outside arbitration it\n", label);
        Serial.printf("[%s]          means CANH or CANL is stuck low (short to GND,\n", label);
        Serial.printf("[%s]          or a missing termination causing reflections).\n", label);
      }
      if (bd1 & (1u << 16)) {
        Serial.printf("[%s]   BIT0-ERR — node sent dominant (0) but bus was recessive (1).\n", label);
        Serial.printf("[%s]          Causes: open-circuit CANH or CANL wire, cable break,\n", label);
        Serial.printf("[%s]          or missing termination on a long cable causing ringing.\n", label);
      }
    }
  };

  if (g_cfg.can0_enable) {
    if (can0_success) { report_bus("CAN0", can0, k_bus_presets[g_bus_preset_idx].is_fd, k_bus_presets[g_bus_preset_idx].arb_bps); }
    else {
      Serial.println("[CAN0] Controller failed to initialise.");
      Serial.println("[CAN0]   Check SPI wiring (MOSI/MISO/SCK/CS), 3.3V supply, and MCP2517FD power.");
    }
  } else {
    Serial.println("[CAN0] Disabled in config.");
  }

  if (g_cfg.can1_enable) {
    if (can1_success) { report_bus("CAN1", can1, k_bus_presets[g_bus_preset_idx].is_fd, k_bus_presets[g_bus_preset_idx].arb_bps); }
    else {
      Serial.println("[CAN1] Controller failed to initialise.");
      Serial.println("[CAN1]   Check SPI wiring (MOSI/MISO/SCK/CS), 3.3V supply, and MCP2517FD power.");
    }
  } else {
    Serial.println("[CAN1] Disabled in config.");
  }

  Serial.println("==========================================");
}

/**
 * @brief Print CAN frame in Kvaser .asc compatible format.
 * @details Importable by CANalyzer, SavvyCAN, and Kvaser Log Converter.
 * @param bus  CAN bus index (0 or 1)
 * @param id   CAN frame ID (11-bit or 29-bit extended)
 * @param len  Data length code
 * @param buf  Data bytes
 */
void print_can_ascii(uint8_t bus, long unsigned int id, uint8_t len, uint8_t *buf) {
  float ts = millis() / 1000.0f;
  bool ext = (id > 0x7FF);
  if (ext) {
    Serial.printf("%10.6f %u  %08lXx             Rx   d %u", ts, (unsigned)(bus + 1u), id, len);
  } else {
    Serial.printf("%10.6f %u  %03lX             Rx   d %u", ts, (unsigned)(bus + 1u), id, len);
  }
  for (int i = 0; i < len; i++) {
    Serial.printf(" %02X", buf[i]);
  }
  Serial.print("\r\n");
}

/**
 * @brief Handle incoming CAN messages with bounded processing.
 * @details Limits messages processed per call to prevent starvation of other tasks.
 */
void handle_incoming_can() {
  messages_processed = 0;

  if (g_cfg.can0_enable && can0.available()) {
    can0MessageProcTime = micros();
    while (can0.receive(frame) && messages_processed < max_messages_per_call) {
      messages_processed++;
      if (can_bridge != nullptr) {
        can_bridge->onMessageReceived(frame, 0);
      }
      if (g_serial_logging && g_serial_mode != 0) {
        print_can_mesg(0, frame.id, frame.len, frame.data);
      }
    }
    if (messages_processed >= max_messages_per_call && can0.available()) {
      if (g_serial_logging && g_serial_mode == 0) { Serial.printf("CAN0: Processed %d messages, more pending\n", messages_processed); }
    }
    perf_stats.update_can0_proc(micros() - can0MessageProcTime, messages_processed);
  }

  if (g_cfg.can1_enable && can1.available()) {
    can1MessageProcTime = micros();
    messages_processed = 0;
    while (can1.receive(frame) && messages_processed < max_messages_per_call) {
      messages_processed++;
      if (can_bridge != nullptr) {
        can_bridge->onMessageReceived(frame, 1);
      }
      if (g_serial_logging && g_serial_mode != 0) {
        print_can_mesg(1, frame.id, frame.len, frame.data);
      }
    }
    if (messages_processed >= max_messages_per_call && can1.available()) {
      if (g_serial_logging && g_serial_mode == 0) { Serial.printf("CAN1: Processed %d messages, more pending\n", messages_processed); }
    }
    perf_stats.update_can1_proc(micros() - can1MessageProcTime, messages_processed);
  }
}

void setup() {
  /* Drive CS HIGH immediately so MCP251863 ignores the bus while SPI is
     being initialised. ACAN2517FD manages CS internally — never pass a CS
     pin to SPI.begin(). */
  pinMode(CS0, OUTPUT); digitalWrite(CS0, HIGH);
  pinMode(CS1, OUTPUT); digitalWrite(CS1, HIGH);

  Serial.begin(115200);
  if (g_serial_logging && g_serial_mode == 0) {
    Serial.println("=========================================================");
    Serial.println("CAN Bridge Analyzer");
    Serial.println("Hardware: Rippletech SPUD v1.1");
    Serial.println("=========================================================");
    Serial.println(ARDUINO_BOARD);
    Serial.print(F("CPU Frequency = "));
    Serial.print(F_CPU / 1000000);
    Serial.println(F(" MHz"));
  }

  delay(5000);

  g_cfg = load_can_config();
  g_bus_preset_idx = g_cfg.bus_preset;
  g_serial_mode = g_cfg.serial_mode;
  if (g_serial_logging && g_serial_mode == 0) {
    const BusPreset& bp = k_bus_presets[g_bus_preset_idx];
    Serial.printf("[CAN] Config: CAN0=%s | CAN1=%s | %s\n",
                  g_cfg.can0_enable ? "ON" : "OFF",
                  g_cfg.can1_enable ? "ON" : "OFF",
                  bp.label);
  }
  can_init();

  if (g_serial_logging && g_serial_mode == 0) { Serial.println("Draining RX buffers before bridge initialization..."); }
  delay(1000);
  drain_can_rx_buffers();

  if (g_cfg.can0_enable && g_cfg.can1_enable) {
    if (can0_success && can1_success) {
      can_bridge = new CANBridge(&can0, &can1);
      can_bridge->begin();
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Bridge mode: CAN0 \xe2\x86\x94 CAN1 (Bidirectional bridge)"); }
    } else if (can0_success) {
      can_bridge = new CANBridge(&can0, nullptr);
      can_bridge->begin();
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Analysis mode: CAN0 (Monitor only - CAN1 failed)"); }
    } else if (can1_success) {
      can_bridge = new CANBridge(&can1, nullptr);
      can_bridge->begin();
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Analysis mode: CAN1 (Monitor only - CAN0 failed)"); }
    } else {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Warning: No CAN controllers initialized successfully"); }
      can_bridge = nullptr;
    }
  } else if (g_cfg.can0_enable) {
    if (can0_success) {
      can_bridge = new CANBridge(&can0, nullptr);
      can_bridge->begin();
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Analysis mode: CAN0 (Monitor only - single controller)"); }
    } else {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Warning: CAN0 controller failed to initialize"); }
      can_bridge = nullptr;
    }
  } else if (g_cfg.can1_enable) {
    if (can1_success) {
      can_bridge = new CANBridge(&can1, nullptr);
      can_bridge->begin();
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Analysis mode: CAN1 (Monitor only - single controller)"); }
    } else {
      if (g_serial_logging && g_serial_mode == 0) { Serial.println("Warning: CAN1 controller failed to initialize"); }
      can_bridge = nullptr;
    }
  } else {
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Warning: No CAN controllers enabled in runtime config"); }
    can_bridge = nullptr;
  }

  if (g_serial_logging && g_serial_mode == 0) { Serial.println("Starting WiFi Access Point..."); }

  if (can_bridge != nullptr) {
    web_server = new CANWebServer(can_bridge);
    web_server->begin(g_cfg.wifi_ssid, g_cfg.wifi_pass[0] ? g_cfg.wifi_pass : nullptr);
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Web server started with CAN bridge"); }
  } else {
    WiFi.mode(WIFI_AP);
    IPAddress local_ip(192, 168, 1, 1);
    IPAddress gateway(192, 168, 1, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP(g_cfg.wifi_ssid, g_cfg.wifi_pass[0] ? g_cfg.wifi_pass : nullptr);
    if (g_serial_logging && g_serial_mode == 0) {
      Serial.println("=== WiFi Access Point Started (CAN disabled) ===");
      Serial.printf("SSID: %s\n", g_cfg.wifi_ssid);
      Serial.printf("Password: %s\n", g_cfg.wifi_pass[0] ? g_cfg.wifi_pass : "(none - open)");
      Serial.printf("IP Address: %s\n", WiFi.softAPIP().toString().c_str());
      Serial.println("Connect to WiFi and navigate to: http://192.168.1.1");
      Serial.println("Note: CAN functionality disabled due to initialization failure");
    }
    web_server = new CANWebServer(nullptr);
    web_server->begin(g_cfg.wifi_ssid, g_cfg.wifi_pass[0] ? g_cfg.wifi_pass : nullptr);
    if (g_serial_logging && g_serial_mode == 0) { Serial.println("Web server started without CAN bridge (debug mode)"); }
  }
  if (g_serial_logging && g_serial_mode == 0) { Serial.println("Setup complete"); }
}

/* -----------------------------------------------------------------------
 * GVRET inbound command handler
 * SavvyCAN sends command packets when it opens the serial connection.
 * We must respond to complete the handshake — the critical one is
 * GET_DEVICE_INFO (0x07): SavvyCAN sets status=CONNECTED when it receives
 * our reply.  Called every loop() when serial_mode==2.
 * ----------------------------------------------------------------------- */

/** @brief Send a GVRET response packet for the given command. */
static void gvret_reply(uint8_t cmd) {
  uint8_t r[16];
  int n = 0;
  uint32_t spd = k_bus_presets[g_bus_preset_idx].arb_bps;

  switch (cmd) {
    case 0x01: {  /* TIME_SYNC — echo back our current micros */
      uint32_t t = (uint32_t)micros();
      r[0]=0xF1; r[1]=0x01;
      r[2]=(uint8_t)t;      r[3]=(uint8_t)(t>>8);
      r[4]=(uint8_t)(t>>16); r[5]=(uint8_t)(t>>24);
      n = 6; break;
    }
    case 0x05:  /* SET_CANBUS_PARAMS — ACK (speed changes require reboot via /config) */
      r[0]=0xF1; r[1]=0x05; n = 2; break;

    case 0x06: {  /* GET_CANBUS_PARAMS */
      r[0]=0xF1; r[1]=0x06;
      r[2]=(uint8_t)spd;      r[3]=(uint8_t)(spd>>8);
      r[4]=(uint8_t)(spd>>16); r[5]=(uint8_t)(spd>>24);
      r[6] = g_cfg.can0_enable ? 0x10 : 0x00;
      r[7]=(uint8_t)spd;      r[8]=(uint8_t)(spd>>8);
      r[9]=(uint8_t)(spd>>16); r[10]=(uint8_t)(spd>>24);
      r[11] = g_cfg.can1_enable ? 0x10 : 0x00;
      n = 12; break;
    }
    case 0x07: {  /* GET_DEVICE_INFO — SavvyCAN sets CONNECTED on receipt */
      r[0]=0xF1; r[1]=0x07;
      r[2]=0x00; r[3]=0x00;
      r[4]=0x00; r[5]=0x00;
      n = 6; break;
    }
    case 0x09:  /* GET_NUM_BUSES */
      r[0]=0xF1; r[1]=0x09; r[2]=2; n = 3; break;

    default: break;
  }

  if (n > 0) Serial.write(r, n);
}

/**
 * @brief Parse and respond to incoming GVRET serial commands from SavvyCAN.
 * @details State machine: waits for 0xF1 start byte, reads command byte,
 *          consumes any fixed payload, then calls gvret_reply().
 *          Must be called every loop() when g_serial_mode == 2.
 */
static void handle_gvret_commands() {
  if (g_serial_mode != 2) {
    while (Serial.available()) (void)Serial.read();
    return;
  }

  static uint8_t gs_state  = 0;
  static uint8_t gs_cmd    = 0;
  static uint8_t gs_remain = 0;

  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();

    if (gs_state == 0) {
      if (b == 0xF1) gs_state = 1;

    } else if (gs_state == 1) {
      gs_cmd = b;
      switch (gs_cmd) {
        case 0x01: gs_remain = 4;  break;
        case 0x05: gs_remain = 10; break;
        default:   gs_remain = 0;  break;
      }
      if (gs_remain == 0) { gvret_reply(gs_cmd); gs_state = 0; }
      else                  gs_state = 2;

    } else {
      if (--gs_remain == 0) { gvret_reply(gs_cmd); gs_state = 0; }
    }
  }
}

void loop() {
  unsigned long loop_start = micros();

  handle_gvret_commands();

  handle_deferred_can_isr();

  unsigned long can_start = micros();
  handle_incoming_can();
  unsigned long can_end = micros();
  perf_stats.update_handle_can(can_end - can_start);

  if (can_bridge != nullptr) {
    can_bridge->update();
  }

  static unsigned long last_web_update = 0;
  const unsigned long web_update_interval_ms = 250;

  if (millis() - last_web_update >= web_update_interval_ms) {
    if (web_server != nullptr) {
      unsigned long ws_start = micros();
      web_server->update();
      unsigned long ws_end = micros();
      perf_stats.update_websocket(ws_end - ws_start);
    }
    last_web_update = millis();
  }

  unsigned long loop_end = micros();
  perf_stats.update_main_loop(loop_end - loop_start);

  unsigned long now = millis();
  if (g_serial_logging && g_serial_mode == 0) {
    static unsigned long last_diag_print = 0;
    if (now - last_diag_print >= 5000) {
      print_can_diagnostics();
      perf_stats.print_stats();
      perf_stats.reset();
      last_diag_print = now;
      perf_stats.last_stats_print = now;
    }
  } else {
    if (now - perf_stats.last_stats_print >= 5000) {
      perf_stats.reset();
      perf_stats.last_stats_print = now;
    }
  }
}
