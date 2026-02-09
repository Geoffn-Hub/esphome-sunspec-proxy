# Quick Reference - Hoymiles Register Mapping

## Register Offset Quick Lookup

Use this when debugging or verifying data:

```
Spec Byte → Modbus Register → Field Name
──────────────────────────────────────────
0x1000    → 0x00 (reg 0)    → Data Type (high byte) / Serial byte 0 (low byte)
0x1002    → 0x01 (reg 1)    → Serial bytes 1-2
0x1004    → 0x02 (reg 2)    → Serial bytes 3-4
0x1006    → 0x03 (reg 3)    → Serial byte 5 (high) / Port Number (low)
0x1008    → 0x04 (reg 4)    → PV Voltage (÷10 → V)
0x100A    → 0x05 (reg 5)    → PV Current (÷100 → A for HM, ÷10 for MI)
0x100C    → 0x06 (reg 6)    → Grid Voltage (÷10 → V)
0x100E    → 0x07 (reg 7)    → Grid Frequency (÷100 → Hz)
0x1010    → 0x08 (reg 8)    → PV Power (÷10 → W)
0x1012    → 0x09 (reg 9)    → Today Production (raw Wh, uint16)
0x1014    → 0x0A (reg 10)   → Total Production HIGH (raw Wh, uint32)
0x1016    → 0x0B (reg 11)   → Total Production LOW
0x1018    → 0x0C (reg 12)   → Temperature (÷10 → °C, signed int16)
0x101A    → 0x0D (reg 13)   → Operating Status
0x101C    → 0x0E (reg 14)   → Alarm Code
0x101E    → 0x0F (reg 15)   → Alarm Count
0x1020    → 0x10 (reg 16)   → Link Status (high byte) / 0x07 (low byte)
0x1022    → 0x11-0x13       → Reserved (regs 17-19)
```

## Conversion Formulas

**Byte Address → Register Offset:**
```
offset = (byte_address - 0x1000) / 2
```

**Port Base Address:**
```
port_base = 0x1000 + (port_number × 0x28)
```

**Decimal Places → Scaling:**
```
decimal = 1  →  ÷10   →  × 0.1
decimal = 2  →  ÷100  →  × 0.01
decimal = /  →  raw   →  no scaling
```

## Code Constants

```cpp
// Base & stride
HM_DATA_BASE = 0x1000          // Port 0 start
HM_PORT_STRIDE = 0x28          // 40 bytes = 20 regs
HM_PORT_REGS = 20              // Read count

// Register offsets (within port)
HM_DATA_TYPE_SN = 0x00         // Data type (high) + SN byte 0 (low)
HM_SN_REG1 = 0x01              // SN bytes 1-2
HM_SN_PORT = 0x03              // SN byte 5 (high) + port (low)
HM_PV_VOLTAGE = 0x04           // ×0.1 → V
HM_PV_CURRENT = 0x05           // ×0.01 → A (HM series)
HM_GRID_VOLTAGE = 0x06         // ×0.1 → V
HM_GRID_FREQ = 0x07            // ×0.01 → Hz
HM_PV_POWER = 0x08             // ×0.1 → W
HM_TODAY_PROD = 0x09           // raw Wh (uint16)
HM_TOTAL_PROD_H = 0x0A         // raw Wh (uint32 high)
HM_TOTAL_PROD_L = 0x0B         // raw Wh (uint32 low)
HM_TEMPERATURE = 0x0C          // ×0.1 → °C (int16)
HM_OPERATING_STATUS = 0x0D
HM_ALARM_CODE = 0x0E
HM_ALARM_COUNT = 0x0F
HM_LINK_STATUS = 0x10          // high byte only

// DTU serial
HM_DTU_SN_BASE = 0x2000        // DTU SN start
HM_DTU_SN_REGS = 3             // 3 regs = 6 bytes
```

## Modbus Function Codes

```
FC 0x03 - Read Holding Registers
  → Use for: 0x1000+ (port data), 0x2000 (DTU serial)

FC 0x01 - Read Coils
FC 0x02 - Read Discrete Inputs
  → Use for: 0xC000+ (status registers)

FC 0x05 - Write Single Coil
  → Use for: 0xC006+ (inverter on/off)

FC 0x06 - Write Single Register
  → Use for: 0xC007+ (power limit)
```

## Example Read Request

```
Port 0 data:
  Address: 0x1000
  Function: 0x03 (Read Holding)
  Count: 20 registers
  
Port 1 data:
  Address: 0x1028 (0x1000 + 1×0x28)
  Function: 0x03
  Count: 20 registers

DTU serial:
  Address: 0x2000
  Function: 0x03
  Count: 3 registers
```

## Expected Values (for validation)

```
PV Voltage:     30-60V (typical)
PV Current:     0-15A (depends on panel)
Grid Voltage:   220-240V (or 110-120V)
Grid Frequency: 49.5-50.5Hz (or 59.5-60.5Hz)
PV Power:       0-2000W (depends on model)
Temperature:    20-80°C
Today Energy:   0-50000Wh (resets at midnight)
Total Energy:   0-999999Wh (never decreases)
```

## Common Mistakes

❌ **Using spec byte addresses as register offsets directly**
   → Always divide byte offset by 2!

❌ **Reading 40 registers per port**
   → Only 20 registers contain data!

❌ **Using FC 0x03 for status registers (0xC000+)**
   → Use FC 0x01/0x02 instead!

❌ **Not scaling values**
   → Must apply ×0.1 or ×0.01 per spec's decimal column!

❌ **Treating temperature as unsigned**
   → It's signed int16 (can be negative in cold climates)!

## Debugging Tips

### Check Raw Registers
Look for patterns in VERBOSE logs:
```
RAW regs 0-9: 0100 1234 5678 90AB 04D2 03E8 0906 1388 04D2 162E
              ^^^^ ^^^^ ^^^^ ^^^^ ^^^^ ^^^^ ^^^^ ^^^^ ^^^^ ^^^^
              type SN   SN   SN   PVV  PVI  GV   GF   PVP  TodP
              +sn0 b1-2 b3-4 b5+p
```

### Verify Scaling
If a value looks wrong, check:
1. Is it 10× too high/low? (Wrong decimal places)
2. Is it 2× too high? (Old code used ÷2 for current)
3. Is it 100× too high/low? (Byte vs register confusion)

### Serial Number Format
Should be 12 hex chars: `1121abcd5678`
If you see ASCII or weird chars, byte extraction is wrong.

## Port Stride Visual

```
Port 0: 0x1000 ─┬─ 20 registers (0x1000-0x1013)
                └─ 40 bytes
                
Port 1: 0x1028 ─┬─ 20 registers (0x1028-0x103B)
                └─ 40 bytes
                
Port 2: 0x1050 ─┬─ 20 registers (0x1050-0x1063)
                └─ 40 bytes
```

Stride: 0x1028 - 0x1000 = 0x28 = 40 bytes = 20 registers

