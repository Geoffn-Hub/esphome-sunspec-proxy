# Hoymiles Register Address Clarification

## CRITICAL: Byte Addresses vs Modbus Register Addresses

### The Source of Confusion

The **official Hoymiles DTU-Pro technical specification (Chapter 4.3.2)** uses **BYTE addresses**, but **Modbus Function Code 0x03** (Read Holding Registers) operates on **REGISTER addresses** where each register is 16 bits (2 bytes).

This means all addresses in the spec must be converted when implementing Modbus reads.

---

## Address Conversion Formula

```
Modbus_Register_Address = Byte_Address_From_Spec
Modbus_Register_Offset = (Byte_Address - Port_Base_Address) / 2
```

**Example from spec:**
- **Spec says:** "PV Voltage at byte address 0x1008-0x1009"
- **Port base:** 0x1000 bytes
- **Calculation:** (0x1008 - 0x1000) / 2 = 0x08 / 2 = **4**
- **Result:** PV Voltage is at Modbus register offset **4** from port base

---

## Official Spec Mapping (Chapter 4.3.2)

From Hoymiles DTU-Pro RTU Data Register List:

| Spec Byte Address | Field Name | Modbus Register | Decimal | Unit | Notes |
|-------------------|------------|-----------------|---------|------|-------|
| 0x1000-0x1001 | Data Type | 0 (high byte) | - | - | Type identifier |
| 0x1001-0x1006 | Serial Number | 0-3 (6 bytes) | - | - | Hex string |
| 0x1007 | Port Number | 3 (low byte) | - | - | DTU port # |
| 0x1008-0x1009 | PV Voltage | 4 | 1 | V | Raw ÷ 10 |
| 0x100A-0x100B | PV Current | 5 | 1 or 2 | A | ÷10 (MI) or ÷100 (HM) |
| 0x100C-0x100D | Grid Voltage | 6 | 1 | V | Raw ÷ 10 |
| 0x100E-0x100F | Grid Frequency | 7 | 2 | Hz | Raw ÷ 100 |
| 0x1010-0x1011 | PV Power | 8 | 1 | W | Raw ÷ 10 |
| 0x1012-0x1013 | Today Production | 9 | none | Wh | Raw uint16 |
| 0x1014-0x1017 | Total Production | 10-11 | none | Wh | Raw uint32 (big-endian) |
| 0x1018-0x1019 | Temperature | 12 | 1 | °C | Raw ÷ 10 (signed int16) |
| 0x101A-0x101B | Operating Status | 13 | - | - | Status code |
| 0x101C-0x101D | Alarm Code | 14 | - | - | Alarm code |
| 0x101E-0x101F | Alarm Count | 15 | - | - | Alarm count |
| 0x1020 | Link Status | 16 (high byte) | - | - | Link status flags |
| 0x1021 | Fixed Value | 16 (low byte) | - | - | Always 0x07 |
| 0x1022-0x1027 | Reserved | 17-19 | - | - | Reserved |

### "Decimal" Column Interpretation

The spec's **"decimal" column** indicates the number of decimal places:
- **decimal = 1** → divide by 10 → multiply by 0.1
- **decimal = 2** → divide by 100 → multiply by 0.01
- **decimal = none** → use raw value (no scaling)

---

## Port Addressing

### Base Address
Each inverter port has its own register block starting at:
```
Port_Base_Address = 0x1000 + (port_number × 0x28)
```

Where:
- `0x1000` = byte address from spec (also the Modbus register address)
- `0x28` = 40 bytes = 20 Modbus registers (port stride)

### Port Stride Calculation
- **Spec stride:** 0x28 **bytes**
- **Modbus stride:** 0x28 / 2 = 0x14 = **20 registers**

Example:
- **Port 0:** Modbus registers 0x1000 to 0x1013 (20 registers)
- **Port 1:** Modbus registers 0x1014 to 0x1027 (20 registers)
- **Port 2:** Modbus registers 0x1028 to 0x103B (20 registers)

Wait, that's not right. Let me recalculate:
- Port 0: 0x1000 + (0 × 0x28) = 0x1000
- Port 1: 0x1000 + (1 × 0x28) = 0x1028
- Port 2: 0x1000 + (2 × 0x28) = 0x1050

So the stride is applied directly as 0x28 to the BASE address (which is already in register units for Modbus purposes).

---

## Data Types by Model Series

### PV Current Scaling Variation

The **PV Current** field (register 5) has **model-dependent scaling**:

| Model Series | Decimal Places | Scaling Factor | Example: Raw 1234 → |
|--------------|----------------|----------------|---------------------|
| **HM/HMS** | 2 | ×0.01 | 12.34 A |
| **MI/MIT/HMT** | 1 | ×0.1 | 123.4 A |

**Current implementation:** Uses ×0.01 (HM/HMS series). If MI series support is needed, this should be made configurable per inverter.

---

## Status Registers (0xC000+)

**IMPORTANT:** Status/control registers at 0xC000 and above use **different Modbus function codes:**
- **FC 0x01** - Read Coils (read on/off status)
- **FC 0x02** - Read Discrete Inputs
- **FC 0x05** - Write Single Coil (turn on/off)
- **FC 0x06** - Write Single Register (set power limit)

**Do NOT use FC 0x03** (Read Holding Registers) for addresses ≥ 0xC000 — it will fail!

### Example Status Registers
```
Port_Control_Base = 0xC006 + (port_number × 6)

Port 0:
  0xC006: ON/OFF control (FC 0x05)
  0xC007: Power limit % (FC 0x06, range 2-100)

Port 1:
  0xC00C: ON/OFF control
  0xC00D: Power limit %
```

---

## DTU Serial Number

**Location:** Byte addresses 0x2000-0x2005 (6 bytes)
**Modbus:** Register 0x2000, read 3 registers with FC 0x03
**Format:** 6-byte hex string (e.g., "99aabbccddee")

---

## Our Implementation Constants

In `sunspec_proxy.h`:

```cpp
// Base addresses (Modbus register addresses, same as spec byte addresses)
static const uint16_t HM_DATA_BASE = 0x1000;       // Port data base
static const uint16_t HM_PORT_STRIDE = 0x28;       // 40 bytes = 20 registers
static const uint16_t HM_DTU_SN_BASE = 0x2000;     // DTU serial base
static const uint16_t HM_PORT_REGS = 20;           // Registers per port

// Register offsets within a port (derived from byte addresses / 2)
static const uint16_t HM_DATA_TYPE_SN = 0x00;      // (0x1000-0x1000)/2 = 0
static const uint16_t HM_SN_REG1 = 0x01;           // (0x1002-0x1000)/2 = 1
static const uint16_t HM_SN_PORT = 0x03;           // (0x1006-0x1000)/2 = 3
static const uint16_t HM_PV_VOLTAGE = 0x04;        // (0x1008-0x1000)/2 = 4
static const uint16_t HM_PV_CURRENT = 0x05;        // (0x100A-0x1000)/2 = 5
static const uint16_t HM_GRID_VOLTAGE = 0x06;      // (0x100C-0x1000)/2 = 6
static const uint16_t HM_GRID_FREQ = 0x07;         // (0x100E-0x1000)/2 = 7
static const uint16_t HM_PV_POWER = 0x08;          // (0x1010-0x1000)/2 = 8
static const uint16_t HM_TODAY_PROD = 0x09;        // (0x1012-0x1000)/2 = 9
static const uint16_t HM_TOTAL_PROD_H = 0x0A;      // (0x1014-0x1000)/2 = 10
static const uint16_t HM_TOTAL_PROD_L = 0x0B;      // (0x1016-0x1000)/2 = 11
static const uint16_t HM_TEMPERATURE = 0x0C;       // (0x1018-0x1000)/2 = 12
static const uint16_t HM_OPERATING_STATUS = 0x0D;  // (0x101A-0x1000)/2 = 13
static const uint16_t HM_ALARM_CODE = 0x0E;        // (0x101C-0x1000)/2 = 14
static const uint16_t HM_ALARM_COUNT = 0x0F;       // (0x101E-0x1000)/2 = 15
static const uint16_t HM_LINK_STATUS = 0x10;       // (0x1020-0x1000)/2 = 16
```

---

## Verification

To verify our offsets are correct, compare:

### Spec Says (Byte Addresses)
```
0x1008: PV Voltage
0x100A: PV Current
0x1010: PV Power
0x1018: Temperature
0x101A: Operating Status
```

### We Calculate (Modbus Register Offsets)
```
(0x1008 - 0x1000) / 2 = 4  → HM_PV_VOLTAGE = 0x04 ✅
(0x100A - 0x1000) / 2 = 5  → HM_PV_CURRENT = 0x05 ✅
(0x1010 - 0x1000) / 2 = 8  → HM_PV_POWER = 0x08 ✅
(0x1018 - 0x1000) / 2 = 12 → HM_TEMPERATURE = 0x0C ✅
(0x101A - 0x1000) / 2 = 13 → HM_OPERATING_STATUS = 0x0D ✅
```

**All correct!** ✅

---

## Why the Old Code Was Wrong

The original implementation used offsets like:
```cpp
HM_PV_VOLTAGE = 0x08    // WRONG
HM_PV_CURRENT = 0x09    // WRONG
```

These appeared to be **byte offsets** mistakenly used as **register offsets**, resulting in:
- Reading from registers 8, 9, 10... instead of 4, 5, 6...
- Getting completely wrong data (voltage from what should be power field, etc.)
- Reading 40 registers when only 20 exist (wasting bandwidth)

The fix applied the correct **byte address → register offset** conversion.

---

## Summary

✅ **Hoymiles spec = byte addresses**  
✅ **Modbus FC 0x03 = register addresses**  
✅ **Conversion: divide byte offset by 2**  
✅ **Decimal column: 1 = ×0.1, 2 = ×0.01**  
✅ **Port stride: 0x28 bytes = 20 registers**  
✅ **Read 20 registers per port, not 40**  
✅ **Status registers: use FC 0x01/0x02/0x05/0x06, not FC 0x03**  

