/** @file can_ids.h
 *  @brief Nissan Leaf (ZE0/AZE0 2011-2017) CARCAN and CHAdeMO CAN ID definitions.
 *
 *  Sources:
 *    - dalathegreat/Battery-Emulator NISSAN-LEAF-BATTERY.cpp (verified decoding)
 *    - openinverter.org Nissan Leaf Gen1 CAN wiki
 *    - CHAdeMO protocol v1.0 (IEC 61851-23)
 *
 *  Bus assignment (this project):
 *    CAN0 = CARCAN (main vehicle CAN, 500 kbps)
 *    CAN1 = EVCAN / PDU bus (CHAdeMO side, 500 kbps)
 *
 *  CHAdeMO strategy:
 *    The PDU (Power Delivery Unit) sits between CARCAN and the CHAdeMO connector.
 *    It reads BMS data from CARCAN (0x1DB, 0x1DC, 0x55B, 0x5BC) and generates
 *    the CHAdeMO protocol frames toward the EVSE. You must supply the correct
 *    CARCAN messages — the PDU handles the CHAdeMO handshake automatically.
 *
 *    Key messages to monitor / inject for CHAdeMO:
 *      Rx from BMS: 0x1DB (voltage/current/status)
 *                   0x1DC (charge power limits  <-- max current to charger)
 *                   0x55B (SOC)
 *                   0x5BC (GIDs / energy remaining)
 *      Tx to BMS:   0x1D4 (VCM charge request keepalive - REQUIRED)
 *                   0x1F2 (VCM charging status - REQUIRED during charge)
 *                   0x50B (VCM wakeup command - keep BMS awake)
 *                   0x50C (VCM alive counter)
 */

#ifndef CAN_IDS_H
#define CAN_IDS_H

/* ============================================================
 *  CARCAN — BMS transmit (every 10-100 ms, received by PDU)
 * ============================================================ */

/** 0x1DB  BMS_Status  10 ms
 *  Key signals:
 *    Current:         bits [7:5] of byte 0 and [7:5] of byte 1
 *                     11-bit two's complement, 0.5 A/bit
 *                     val = ((buf[0]<<3) | (buf[1]>>5)); if (val & 0x400) val |= 0xF800;
 *                     amps = val * 0.5f
 *    Pack voltage:    bits [7:0] of byte 2 and bits [7:6] of byte 3
 *                     10-bit, 0.5 V/bit
 *                     val = (buf[2]<<2) | (buf[3]>>6);  volts = val * 0.5f
 *    Relay cut req:   buf[1] bits [4:3]  (0=normal, 1=normal stop, 2=charge stop)
 *    Failsafe status: buf[1] bits [2:0]  (0=normal; 1-7=stop requests)
 *    Main relay ON:   buf[3] bit 5
 *    Full charge:     buf[3] bit 4
 *    Interlock:       buf[3] bit 3  (both plugs seated)
 *    CRC:             buf[7]  (Nissan proprietary XOR)                           */
#define LEAF_ID_BMS_STATUS          0x1DB

/** 0x1DC  BMS_ChargePowerLimits  10 ms
 *  Key signals:
 *    Max discharge power:   (buf[0]<<2 | buf[1]>>6) / 4.0   kW  (0.25 kW/bit)
 *    Max charge power:      ((buf[1]&0x3F)<<4 | buf[2]>>4) / 4.0  kW
 *    Max charger power:     (((buf[2]&0x0F)<<6 | buf[3]>>2) / 10.0) - 10  kW
 *                           << THIS is what the PDU uses to set the CHAdeMO
 *                              max charge current to the EVSE
 *    CRC: buf[7]                                                                 */
#define LEAF_ID_BMS_CHARGE_LIMITS   0x1DC

/** 0x55B  BMS_SOC  100 ms
 *  Key signals:
 *    SOC:          (buf[0]<<2 | buf[1]>>6),  10-bit, 0.1 %/bit  (0-1000 = 0-100%)
 *    Capacity empty: buf[6] bit 7
 *    CRC: buf[7]                                                                 */
#define LEAF_ID_BMS_SOC             0x55B

/** 0x5BC  BMS_GIDS  100 ms
 *  Key signals:
 *    GIDS (remaining energy): buf[0]<<2 | buf[1]>>6,  10-bit
 *                             1 GID = 80 Wh  (multiply by 80 for Wh)
 *                             0-281 = 24kWh, 0-363 = 30kWh, 0-498 = 40kWh, 0-775 = 62kWh
 *    Max GIDS (pack size):    same decode when buf[5] bit 4 = 1 (MUX bit)
 *    SOH:                     buf[4] >> 1,  7-bit, 1 %/bit  (0-99%)
 *    Avg temperature (ZE0 only): buf[3] - 40  (deg C);  AZE0/ZE1 use MUX segments
 *    BMS alive indicator: presence of this frame                                 */
#define LEAF_ID_BMS_GIDS            0x5BC

/** 0x5C0  BMS_HistData  100 ms  (AZE0 2013-2017 only)
 *  Key signals:
 *    MUX:         buf[0] bits [7:6]  (1=MAX temp, 3=MIN temp)
 *    Temperature: buf[2] / 2 - 40   (deg C, valid for active MUX)
 *    Heater exists: buf[4] bit 0
 *    Heating start: buf[0] bit 5
 *    Heating stop:  buf[0] bit 4                                                 */
#define LEAF_ID_BMS_HIST_DATA       0x5C0

/** 0x1ED  ZE1_BMS_62kWh  10 ms  (ZE1 2018+ with 62kWh battery only)
 *  Key signals:
 *    MAX_POWER_FOR_CHARGER: bits [7:0] Motorola 11-bit = 0.1 kW/bit + 10 kW offset
 *                           (range -10 to +204 kW; used to set CHAdeMO charge limit) */
#define LEAF_ID_ZE1_BMS_62KWH       0x1ED

/** 0x59E  BMS_QC_Capacity  100 ms  (AZE0 2013+ / ZE1; absent on ZE0 2011-2012)
 *  Presence indicates AZE0 or ZE1 battery. Also carries quick-charge capacity:
 *    LB_Full_Capacity_for_QC:   bits [20:12] Motorola = 100 Wh/bit  (full pack Wh)
 *    LB_Remain_Capacity_for_QC: bits [27:19] Motorola = 100 Wh/bit  (remaining Wh)
 *  ZE1 62kWh adds LB_Full_Capacity_for_QC_62 at bits [30:21] Motorola.          */
#define LEAF_ID_BMS_QC_CAPACITY     0x59E

/* ============================================================
 *  CARCAN — VCM transmit (must be sent to keep BMS alive)
 * ============================================================ */

/** 0x1D4  VCM_ChargeRequest  10 ms
 *  Rotating keepalive required by BMS. Without it BMS will not allow contactors.
 *  Byte [4] and [7] cycle through 4 patterns:
 *    case 0: buf[4]=0x07, buf[7]=0x12
 *    case 1: buf[4]=0x47, buf[7]=0xD5
 *    case 2: buf[4]=0x87, buf[7]=0x19
 *    case 3: buf[4]=0xC7, buf[7]=0xDE                                           */
#define LEAF_ID_VCM_CHARGE_REQ      0x1D4

/** 0x1F2  VCM_ChargingStatus  10 ms
 *  20-state rotating pattern in bytes [6][7]. Byte [3] alternates 0xB0/0xB4.
 *  Required during charging — tells BMS that a charge session is active.        */
#define LEAF_ID_VCM_CHARGE_STATUS   0x1F2

/** 0x50B  VCM_WakeupSleep  100 ms
 *  buf[0] bits [7:6]:  0b11 = wake up BMS,  0b00 = sleep
 *  Must be 0b11 during CHAdeMO session.                                         */
#define LEAF_ID_VCM_WAKEUP          0x50B

/** 0x50C  VCM_Counter  100 ms
 *  Alive counter in buf[3] (0-3 rotating). Required by BMS.                     */
#define LEAF_ID_VCM_COUNTER         0x50C

/* ============================================================
 *  CARCAN — PDU / CHAdeMO related
 * ============================================================ */

/** 0x3B2  PDU_ChargeRequest  100 ms
 *  Generated by PDU onto CHAdeMO CAN. Monitoring this on CARCAN shows whether
 *  the PDU has started a CHAdeMO session.
 *  buf[0]:      Target charge current (A, 0-125 A gen1, 0-200 A gen2)
 *  buf[1:2]:    Target charge voltage (V x 2)
 *  buf[3] bit7: Charge enable
 *  buf[3] bit6: Fault
 *  buf[3] bit5: Sequence toggle                                                  */
#define LEAF_ID_PDU_CHARGE_REQUEST  0x3B2

/** 0x3B8  ZE1 BMS keepalive  100 ms  (ZE1 2018-2023 only)
 *  Counter in buf[2] (0-14). buf[1] alternates 0xC8/0xE8.
 *  Removes DTC U1000 and P318E if absent.                                       */
#define LEAF_ID_ZE1_KEEPALIVE       0x3B8

/** 0x5EC  ZE1 keepalive  500 ms                                                 */
#define LEAF_ID_ZE1_500MS           0x5EC

/** 0x5C5  ZE1 keepalive  100 ms  (removes DTC U214E)                            */
#define LEAF_ID_ZE1_5C5             0x5C5

/** 0x626  ZE1 keepalive  100 ms  (removes DTC U215B)                            */
#define LEAF_ID_ZE1_626             0x626

/* ============================================================
 *  CARCAN — ZE1 CHAdeMO mirror IDs  (100 ms each)
 *  The ZE1 (2018-2023) OBC mirrors the CHAdeMO QC-CAN protocol frames
 *  back onto CARCAN at these IDs so the VCM can observe the session.
 *  ZE0 / AZE0 do NOT produce these frames.
 * ============================================================ */

/** 0x3B9  ZE1 mirror of QC-CAN 0x100  (EV→EVSE: max battery voltage, charge rate) */
#define LEAF_ID_ZE1_QC_EV_CAP       0x3B9

/** 0x3BB  ZE1 mirror of QC-CAN 0x101  (EV→EVSE: max charge time, rated capacity)  */
#define LEAF_ID_ZE1_QC_EV_TIME      0x3BB

/** 0x3BC  ZE1 mirror of QC-CAN 0x102  (EV→EVSE: target voltage, current request)  */
#define LEAF_ID_ZE1_QC_EV_REQ       0x3BC

/** 0x3BE  ZE1 mirror of QC-CAN 0x200  (EV→EVSE: V2H discharge parameters)        */
#define LEAF_ID_ZE1_QC_V2H          0x3BE

/** 0x3C8  ZE1 mirror of QC-CAN 0x108  (EVSE→EV: available output voltage/current) */
#define LEAF_ID_ZE1_QC_EVSE_OUT     0x3C8

/** 0x3C9  ZE1 mirror of QC-CAN 0x109  (EVSE→EV: output status, remaining time)   */
#define LEAF_ID_ZE1_QC_EVSE_STAT    0x3C9

/** 0x4BC  ZE1 mirror of QC-CAN 0x700  (automaker optional content)               */
#define LEAF_ID_ZE1_QC_AUTOMAKER    0x4BC

/* ============================================================
 *  CARCAN — On-Board Charger (OBC)
 * ============================================================ */

/** 0x380  ZE0 OBC status  100 ms  (ZE0 2011-2012 only)
 *  Key signals:
 *    Quick_Charger_IR_Sensor_Flag:     buf[1] bit 0  (0=absent, 1=present)
 *    Charger_Output_Power:             bits [24:16] = 0.1 kW/bit
 *    Normal_Charger_Relay_Status_Flag: buf[4] bit 6  (0=Off, 1=On)
 *    Quick_Charger_Relay_Status_Flag:  buf[4] bit 5  (0=Off, 1=On)
 *    AC_Voltage:                       bits [50:42] = 0.5 V/bit + 70 V offset   */
#define LEAF_ID_ZE0_OBC_STATUS      0x380

/** 0x5BF  ZE0 OBC J1772 / QC  100 ms  (ZE0 2011-2012 only)
 *  Key signals:
 *    J1772CurrentLimiter: buf[2] = 0.5 A/bit  (max pilot current; 0 if unplugged)
 *    Charger_Status:      buf[4]  (40=IDLE, 48=TIMER_WAIT, 64=FINISHED,
 *                                  96=CHARGING, 176=QUICK_CHARGING)
 *    QC_Voltage:          buf[3] = 1 V/bit + 257 V offset                       */
#define LEAF_ID_ZE0_OBC_J1772       0x5BF

/** 0x390  AZE0/ZE1 OBC status  100 ms  (AZE0 2013-2017 and ZE1 2018-2023)
 *  Key signals:
 *    OBC_Status_AC_Voltage:              bits [28:27]  (0=no signal, 1=100V,
 *                                                       2=200V, 3=abnormal)
 *    OBC_Flag_QC_Relay_On_Announcement:  bit 38        (1=announce off, 2=on)
 *    OBC_Flag_QC_IR_Sensor:              bit 47        (0=without, 1=with)
 *    OBC_Maximum_Charge_Power_Out:       bits [48:40]  = 0.1 kW/bit (MAXCHGPOUT)
 *    OBC_Charge_Power:                   bits [8:0]    = 0.1 kW/bit (actual output)
 *    OBC_Charge_Status:                  bits [45:40]  (see VAL_ table)          */
#define LEAF_ID_OBC_STATUS          0x390

/** 0x393  AZE0/ZE1 OBC secondary  100 ms  (AZE0 2013-2017 and ZE1 2018-2023)
 *  Contains additional OBC state. CSUM = (all nibbles summed) - 1.              */
#define LEAF_ID_OBC_SECONDARY       0x393

/* ============================================================
 *  OBD / Diagnostic
 * ============================================================ */

/** 0x7BB  OBD BMS diagnostic response  (polled every 10 s)
 *  Multi-frame ISO 15765-2. Groups polled:
 *    0x01 - High precision SOC, current, pack voltage, insulation resistance
 *    0x02 - Cell voltages (96 cells, 2 bytes each, mV)
 *    0x04 - Temperature sensors (4 sensors, raw Fahrenheit encoding)
 *    0x06 - Cell balancing shunt status (96 bits)
 *    0x83 - Battery part number (ASCII)
 *    0x84 - Battery serial number (ASCII)
 *    0x90 - BMS ID code (ASCII)                                                 */
#define LEAF_ID_OBD_BMS_RESPONSE    0x7BB

/** 0x79B  LeafSpy polling detect
 *  Seeing this means LeafSpy (or similar) is active. Suspend your own 0x7BB
 *  polling to avoid collisions.                                                  */
#define LEAF_ID_LEAFSPY_DETECT      0x79B

/* ============================================================
 *  CHAdeMO QC-CAN bus IDs  (on the CHAdeMO connector CAN bus)
 *  These are NOT on CARCAN — the PDU / OBC bridges them.
 *  ZE1 also mirrors these onto CARCAN (see LEAF_ID_ZE1_QC_* above).
 *
 *  EV→EVSE direction (vehicle to charger):
 *    0x100  Vehicle capabilities (max battery voltage, charging rate)
 *    0x101  Max charging time permitted by EV
 *    0x102  Charging request (target voltage, current request, status)
 *
 *  EVSE→EV direction (charger to vehicle):
 *    0x108  Charger output params (available voltage/current, threshold)
 *    0x109  Charger output status (actual output, remaining time)
 *
 *  V2H (Vehicle-to-Home) discharge:
 *    0x200  V2H discharge parameters (EV→EVSE)
 * ============================================================ */

/** 0x100  EV_Capability  100 ms  (EV→EVSE)
 *  Key signals (CHAdeMO v1.0+):
 *    MinimumChargeCurrent:             bits [7:0]   = 1 A/bit
 *    MaximumBatteryVoltage:            bits [47:32] = 0.01 V/bit  (max 600 V)
 *    ConstantOfChargingRateIndication: bits [55:48] = 1 %/bit (fixed at 100%)   */
#define CHADEMO_ID_EV_CAPABILITY    0x100

/** 0x101  EV_MaxTime  100 ms  (EV→EVSE)
 *  Key signals:
 *    MaxChargingTime10sBit:   bits [15:8]  = 10 s/bit
 *    MaxChargingTime1minBit:  bits [23:16] = 1 min/bit
 *    EstimatedChargingTime:   bits [31:24] = 1 min/bit  (added CHAdeMO 1.0.1)
 *    RatedBatteryCapacity:    bits [47:32] = 0.11 kWh/bit (added CHAdeMO 1.0.1) */
#define CHADEMO_ID_EV_MAX_TIME      0x101

/** 0x102  EV_ChargeRequest  100 ms  (EV→EVSE)
 *  Key signals:
 *    ControlProtocolNumberEV:      bits [7:0]   = version number
 *    TargetBatteryVoltage:         bits [23:8]  = 0.01 V/bit
 *    ChargingCurrentRequest:       bits [31:24] = 1 A/bit
 *    StatusVehicleCharging:        bit 40  (0=disabled, 1=enabled)
 *    StatusVehicleShifterPosition: bit 41  (0=parked, 1=other)
 *    StatusChargingSystem:         bit 42  (0=normal, 1=fault)
 *    StatusVehicle:                bit 43  (0=contactor closed, 1=open)
 *    StatusNormalStopRequest:      bit 44  (0=no request, 1=stop request)
 *    ChargingRate:                 bits [55:48] = 1 %/bit                       */
#define CHADEMO_ID_EV_CHARGE_REQ    0x102

/** 0x108  EVSE_Output  100 ms  (EVSE→EV)
 *  Key signals:
 *    EVContactorWeldingDetection: bits [7:0]   (0=not supported, 1=supported)
 *    AvailableOutputVoltage:      bits [23:8]  = 0.01 V/bit  (max 600 V)
 *    AvailableOutputCurrent:      bits [31:24] = 1 A/bit
 *    ThresholdVoltage:            bits [47:32] = 0.01 V/bit                     */
#define CHADEMO_ID_EVSE_OUTPUT      0x108

/** 0x109  EVSE_Status  100 ms  (EVSE→EV)
 *  Key signals:
 *    ControlProtocolNumberQC:      bits [7:0]   = version number
 *    OutputVoltage:                bits [23:8]  = 0.01 V/bit
 *    OutputCurrent:                bits [31:24] = 1 A/bit
 *    StatusStation:                bit 40  (0=standby, 1=charging)
 *    FaultStationMalfunction:      bit 41  (0=normal, 1=fault)
 *    StatusVehicleConnectorLock:   bit 42  (0=unlocked, 1=locked)
 *    FaultBatteryIncompatibility:  bit 43  (0=compatible, 1=not compatible)
 *    FaultChargingSystemMalfunction: bit 44
 *    StatusChargerStopControl:     bit 45  (0=operating, 1=shutdown)
 *    RemainingChargingTime10sBit:  bits [55:48] = 10 s/bit
 *    RemainingChargingTime1minBit: bits [63:56] = 1 min/bit                     */
#define CHADEMO_ID_EVSE_STATUS      0x109

/** 0x200  V2H_DischargeParams  100 ms  (EV→EVSE, Vehicle-to-Home)
 *  Key signals:
 *    MaximumDischargeCurrent:      bits [7:0]   = 1 A/bit
 *    MinimumDischargeVoltage:      bits [47:32] = 0.01 V/bit
 *    MinimumBatteryDischargeLevel: bits [55:48] (% or kWh)
 *    MaxRemainingCapacityForCharging: bits [63:56] (optional)                   */
#define CHADEMO_ID_V2H_DISCHARGE    0x200

/* ============================================================
 *  SPUD VCU → Dash / Dash → VCU messages
 *  Private CAN bus between the SPUD VCU (Teensy) and the dash unit.
 *  All frames are 8 bytes, no checksum/CRC.
 *  Defined in: src/leaf/libraries/common/common.h
 * ============================================================ */

/** 0x61A  DASH_STATUS  ~100 ms  (Dash → VCU)
 *  Byte 0 [7:4]: status
 *  Byte 1 [4]:   bmodeRequested
 *  Byte 1 [2]:   cruiseConRequested
 *  Byte 2:       maxCharge                                                      */
#define SPUD_ID_DASH_STATUS         0x61A

/** 0x62A  VCU_DASHMESG1  100 ms  (VCU → Dash)
 *  HV batt voltage   [10:0]  bits [10:0]   = 0.1 V/bit (hV)
 *  HV current        [10:0]  bits [21:11]  = 0.1 A/bit, offset -1024 (hA)
 *  Motor RPM         [11:0]  bits [33:22]  = 10 RPM/bit, offset -1024
 *  Throttle pos      [7:0]   bits [41:34]  = d%
 *  Speed             [11:0]  bits [53:42]  = dKPH
 *  VCU state         [3:0]   bits [57:54]
 *  Brake on          bit 63
 *  Counter           bits [1:0] of byte 7 (0–3 rotating)                       */
#define SPUD_ID_VCU_DASHMESG1       0x62A

/** 0x62B  VCU_DASHMESG2  100 ms  (VCU → Dash)
 *  Byte 0: MC relay[7], run switch[6], neg relay[5], pos relay[3],
 *          EVSE plugged[2], EVSE on[1]
 *  Byte 1: gear[7:5], bmode[4], limp mode[3], cruise active[2]
 *  Byte 2: LV voltage (dV)
 *  Bytes 3–4: SoC (10-bit)
 *  Bytes 4–5: charge rate (11-bit, hA = 0.5 A/bit)
 *  Bytes 5–6: brake vacuum (8-bit)
 *  Bytes 6–7: gear switch voltage (8-bit)
 *  Byte 7 [1:0]: counter (0–3 rotating)                                         */
#define SPUD_ID_VCU_DASHMESG2       0x62B

/** 0x62C  VCU_DASHMESG3  100 ms  (VCU → Dash)
 *  Bytes 0–2: odometer / hour-meter (dkm or dhr, unsigned long)
 *  Byte 3:    inverter temp (hdeg C)
 *  Byte 4:    motor temp (hdeg C)
 *  Byte 5:    min batt temp (hdeg C, offset -50)
 *  Byte 6:    max batt temp (hdeg C, offset -50)
 *  Byte 7 [1:0]: counter (0–3 rotating)                                         */
#define SPUD_ID_VCU_DASHMESG3       0x62C

/** 0x62D  VCU_DASHMESG4  ~500 ms  (VCU → Dash)
 *  Bits [12:0]  bytes 0–1:   min cell voltage (13-bit, mV)
 *  Bits [25:13] bytes 1–3:   max cell voltage (13-bit, mV)
 *  Bits [45:32] bytes 3–5:   energy remaining = 80 × 13-bit value (Wh, GID-based)
 *  Bits [58:46] bytes 4–6:   total capacity   = 80 × 13-bit value (Wh, GID-based)
 *  Byte 7 [1:0]: counter (0–3 rotating)                                         */
#define SPUD_ID_VCU_DASHMESG4       0x62D

/** 0x63A  DTC_BROADCAST  2000 ms  (VCU → Dash)
 *  Byte 0:     lamp status
 *  Byte 1:     DTC type
 *  Bytes 2–3:  DTC code (= RT warning/error code + 256)
 *  Bytes 4–7:  spare                                                            */
#define SPUD_ID_DTC_BROADCAST       0x63A

/* ============================================================
 *  Scaling constants
 * ============================================================ */
#define LEAF_CURRENT_SCALE_A        0.5f    ///< A per bit — 0x1DB current field
#define LEAF_VOLTAGE_SCALE_V        0.5f    ///< V per bit — 0x1DB voltage field
#define LEAF_SOC_SCALE_PCT          0.1f    ///< % per bit — 0x55B SOC field
#define LEAF_POWER_COARSE_KW        0.25f   ///< kW per bit — 0x1DC discharge/charge limit
#define LEAF_POWER_FINE_KW          0.1f    ///< kW per bit — 0x1DC max charger power
#define LEAF_POWER_OFFSET_KW       -10.0f   ///< kW offset  — 0x1DC max charger power
#define LEAF_WH_PER_GID             80.0f   ///< Wh per GID — 0x5BC energy remaining (DBC: "1 LSB = 80WH")
#define LEAF_TEMP_OFFSET_C          40.0f   ///< deg C offset — 0x5BC ZE0 temperature

#endif /* CAN_IDS_H */
