# Verification Checklist - Hoymiles Register Fix

## Files Modified

✅ **sunspec_proxy.h** - Fixed register constants, added DTU fields
✅ **sunspec_proxy.cpp** - Fixed scaling, serial parsing, aggregation, added DTU polling
✅ **__init__.py** - Added DTU sensor config options

---

## Changes Summary

### 1. Register Offset Corrections (sunspec_proxy.h)

```cpp
// OLD (WRONG)                    // NEW (CORRECT)
HM_PV_VOLTAGE = 0x08       →     HM_PV_VOLTAGE = 0x04
HM_PV_CURRENT = 0x09       →     HM_PV_CURRENT = 0x05
HM_GRID_VOLTAGE = 0x0A     →     HM_GRID_VOLTAGE = 0x06
HM_GRID_FREQ = 0x0B        →     HM_GRID_FREQ = 0x07
HM_PV_POWER = 0x0C         →     HM_PV_POWER = 0x08
HM_TODAY_PROD = 0x0D-0x0E  →     HM_TODAY_PROD = 0x09 (single reg)
HM_TOTAL_PROD = 0x0F-0x10  →     HM_TOTAL_PROD_H/L = 0x0A-0x0B
HM_TEMPERATURE = 0x11      →     HM_TEMPERATURE = 0x0C
HM_OPERATING_STATUS = 0x1E →     HM_OPERATING_STATUS = 0x0D
HM_ALARM_CODE = 0x1F       →     HM_ALARM_CODE = 0x0E
HM_LINK_STATUS = 0x20      →     HM_LINK_STATUS = 0x10
HM_PORT_REGS = 0x28 (40)   →     HM_PORT_REGS = 20
```

### 2. Scaling Factor Corrections (sunspec_proxy.cpp)

```cpp
// OLD (WRONG)                           // NEW (CORRECT - Hoymiles spec)
pv_voltage = raw                   →     pv_voltage = raw × 0.1
pv_current = raw / 2.0             →     pv_current = raw × 0.01
grid_voltage = raw                 →     grid_voltage = raw × 0.1
frequency = raw / 100.0            →     frequency = raw × 0.01
pv_power = raw                     →     pv_power = raw × 0.1
today_energy = raw (32-bit)        →     today_energy = raw (16-bit, no scaling)
total_energy = raw (32-bit)        →     total_energy = raw (32-bit, no scaling)
temperature = raw (int16)          →     temperature = raw × 0.1 (signed)
```

### 3. Serial Number Byte-Unpacking Fix

**Old:** Read from regs 1-6 (wrong, ASCII assumption)  
**New:** Extract bytes 1-6 from regs 0-3 (correct, byte-packed big-endian)

```cpp
// Reg 0: [data_type][sn_byte_0]
// Reg 1: [sn_byte_1][sn_byte_2]
// Reg 2: [sn_byte_3][sn_byte_4]
// Reg 3: [sn_byte_5][port_number]
```

### 4. Aggregation Logic Fix

**Old:** Tried to read `s.raw_regs` using SunSpec Model 101/103 offsets → wrong format!  
**New:** Use pre-decoded float values (s.power_w, s.voltage_v, etc.)

### 5. DTU Diagnostics Added

New sensors:
- `dtu_serial` (text) - DTU serial number from register 0x2000
- `dtu_online` (binary) - DTU responding (last poll < 30s ago)
- `dtu_poll_success` (counter) - Successful DTU communications
- `dtu_poll_fail` (counter) - Failed DTU communications

New polling logic:
- On startup: read DTU serial from 0x2000
- Every 60s: poll DTU as alive check (separate from inverter polling)

---

## Compilation Check

The code should compile without errors. Key points:

- ✅ All old constant names removed (HM_DATA_TYPE, HM_SN_START, HM_PORT_NUM, HM_TODAY_PROD_H/L)
- ✅ New constants added (HM_DATA_TYPE_SN, HM_SN_REG1, HM_SN_PORT, HM_TODAY_PROD, HM_ALARM_COUNT, HM_DTU_SN_BASE, HM_DTU_SN_REGS)
- ✅ DTU fields initialized with `{}` (C++11 zero-init)
- ✅ Phase marker updated (current_poll_phase_ comment: -1=DTU, 0=inverter)
- ✅ DTU response handler checks `current_poll_phase_ == -1`

---

## Test Plan

### Expected Behavior After Fix

1. **Voltage readings** → ~230V (not 2300V or 23V)
2. **Current readings** → Reasonable I = P/V (not 2× too high)
3. **Frequency** → 50Hz or 60Hz (not 5000Hz)
4. **PV power** → Should match grid power ±5% (microinverter efficiency)
5. **Today energy** → Resets at midnight, increments during day
6. **Total energy** → Never decreases, survives restarts
7. **Temperature** → Realistic °C with decimal (e.g., 45.3°C, not 453)
8. **Serial numbers** → 12-char hex strings (e.g., "1121xxxxxxxx")
9. **DTU serial** → Appears in Home Assistant after first poll
10. **Aggregate values** → Sum correctly across all inverters

### Debug Logging

Set ESPHome log level to VERBOSE for first boot:

```yaml
logger:
  level: VERBOSE
  logs:
    sunspec_proxy: VERBOSE
```

Look for:
```
[sunspec_proxy] RTU RX: 'HMS-2000-4T-P0' (port 0) — P=1234W (PV: 123.4V/10.00A=1234W), Grid: 230V/50.00Hz, T=45.3°C, Today=5678Wh, Total=123.4kWh, Status=0x0001, Alarm=0/0, Link=0x01
[sunspec_proxy] RAW regs 0-9: 0100 1234 5678 90AB CDEF ...
[sunspec_proxy] DTU: Serial number: 99aabbccddee (poll OK count: 1)
[sunspec_proxy] AGG: P=2468W (L1:1234 L2:1234 L3:0) I=10.72A V=230.0/230.0/0.0V f=50.00Hz E=246.8kWh [2/2, MPPT]
```

---

## Rollback Plan (if needed)

If issues arise:

1. **Restore from backup** (if you took one)
2. **Or revert Git commit** (if using version control)
3. **Or restore old constants:**

```cpp
// Emergency rollback values (NOT RECOMMENDED - these are wrong!)
static const uint16_t HM_PV_VOLTAGE = 0x08;
static const uint16_t HM_PV_CURRENT = 0x09;
static const uint16_t HM_PORT_REGS = 0x28;
// ... etc (see old code)
```

**Note:** Rollback will restore incorrect data, so only use temporarily while investigating issues.

---

## Success Criteria

✅ Code compiles without warnings  
✅ ESPHome device boots successfully  
✅ All inverter sensors publish realistic values  
✅ DTU serial sensor appears in Home Assistant  
✅ Victron GX device sees sensible aggregated power  
✅ No CRC errors or timeouts in logs  
✅ Alarm codes/status registers show valid data  

---

## Common Issues & Solutions

**Issue:** All sensors show 0 or NaN  
**Solution:** Check RS-485 wiring, DTU address (default 126), UART config

**Issue:** Values still look wrong (but different than before)  
**Solution:** Some inverter models (MIT series) may use different scaling - check Hoymiles docs for your model

**Issue:** DTU serial not appearing  
**Solution:** DTU address may not be 126 - check your DTU's Modbus settings

**Issue:** CRC errors in logs  
**Solution:** RS-485 termination resistors needed, or baud rate mismatch (should be 9600)

**Issue:** Compilation error about `current_poll_phase_`  
**Solution:** Ensure header comment matches implementation (phase -1 = DTU poll)

---

## Next Steps

1. **Flash updated firmware** to ESPHome device
2. **Monitor logs** at VERBOSE level for first 5-10 minutes
3. **Verify sensor values** in Home Assistant
4. **Test Victron integration** (if using ESS/DVCC power control)
5. **Run for 24 hours** to verify daily energy reset behavior
6. **Update Home Assistant automations** if needed (thresholds may have changed)

