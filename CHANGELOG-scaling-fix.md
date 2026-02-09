# SunSpec Proxy - Hoymiles Register Scaling Fix

## Date: 2025-06-XX
## Version: v1.3 (post-fix)

## Summary
Fixed critical bugs in Hoymiles DTU-Pro Modbus register parsing that caused incorrect inverter data readings. The previous implementation had wrong register offsets and scaling factors that didn't match the official Hoymiles protocol specification.

---

## Changes Made

### 1. Fixed Register Offset Constants (`sunspec_proxy.h`)

**BEFORE (WRONG):**
```cpp
HM_PV_VOLTAGE = 0x08    // Was offset 8
HM_PV_CURRENT = 0x09    // Was offset 9
HM_GRID_VOLTAGE = 0x0A  // Was offset 10
// ... etc
HM_OPERATING_STATUS = 0x1E  // Was offset 30
HM_ALARM_CODE = 0x1F        // Was offset 31
HM_PORT_REGS = 0x28         // Was reading 40 registers
```

**AFTER (CORRECT):**
```cpp
HM_PV_VOLTAGE = 0x04        // Correct offset 4
HM_PV_CURRENT = 0x05        // Correct offset 5
HM_GRID_VOLTAGE = 0x06      // Correct offset 6
// ... etc
HM_OPERATING_STATUS = 0x0D  // Correct offset 13
HM_ALARM_CODE = 0x0E        // Correct offset 14
HM_PORT_REGS = 20           // Correct: only 20 registers
```

**Why this matters:** The DTU returns data in a packed 20-register block per inverter port. The old code was reading from the wrong offsets, causing:
- Voltage/current/power values to be read from wrong registers
- Status/alarm data to be completely missing or wrong
- Wasted bandwidth reading 40 registers when only 20 exist

---

### 2. Fixed Scaling Factors (`sunspec_proxy.cpp`)

**BEFORE (WRONG):**
```cpp
s.pv_current_a = (float)reg_data[HM_PV_CURRENT] / 2.0f;  // Wrong: ÷2
s.frequency_hz = (float)reg_data[HM_GRID_FREQ] / 100.0f; // Wrong: ÷100
s.pv_voltage_v = (float)reg_data[HM_PV_VOLTAGE];         // Wrong: no scaling
// etc.
```

**AFTER (CORRECT - per Hoymiles spec):**
```cpp
s.pv_voltage_v = (float)reg_data[HM_PV_VOLTAGE] * 0.1f;    // ×0.1 → V
s.pv_current_a = (float)reg_data[HM_PV_CURRENT] * 0.01f;   // ×0.01 → A
s.voltage_v = (float)reg_data[HM_GRID_VOLTAGE] * 0.1f;     // ×0.1 → V
s.frequency_hz = (float)reg_data[HM_GRID_FREQ] * 0.01f;    // ×0.01 → Hz
s.pv_power_w = (float)reg_data[HM_PV_POWER] * 0.1f;        // ×0.1 → W
s.today_energy_wh = (float)reg_data[HM_TODAY_PROD];        // Raw Wh (no scaling)
s.temperature_c = (float)(int16_t)reg_data[HM_TEMPERATURE] * 0.1f;  // ×0.1 → °C (signed)
```

**Why this matters:** The official Hoymiles protocol uses fixed-point arithmetic with specific scale factors. Using the wrong factors caused:
- Current readings off by 2× (too high)
- Frequency readings off by 100× (reading as 5000 Hz instead of 50 Hz)
- Voltage/power readings unscaled (showing raw register values)
- Temperature as integers instead of decimals

---

### 3. Fixed Serial Number Extraction

**BEFORE (WRONG):**
```cpp
// Serial from regs 0x01-0x06 (6 regs = 12 chars)
for (int i = 0; i < 6 && (1 + i) < reg_count; i++) {
    sn[i * 2]     = (reg_data[1 + i] >> 8) & 0xFF;
    sn[i * 2 + 1] = reg_data[1 + i] & 0xFF;
}
```

**AFTER (CORRECT - byte-packed across regs 0-3):**
```cpp
// Serial: bytes 1-6 span registers 0-3 in big-endian byte order
// Reg 0: [data_type(u8)][serial_byte_0]
// Reg 1: [serial_byte_1][serial_byte_2]
// Reg 2: [serial_byte_3][serial_byte_4]
// Reg 3: [serial_byte_5][port_number(u8)]
uint8_t sn_bytes[6];
sn_bytes[0] = reg_data[HM_DATA_TYPE_SN] & 0xFF;       // low byte of reg 0
sn_bytes[1] = (reg_data[HM_SN_REG1] >> 8) & 0xFF;     // high byte of reg 1
sn_bytes[2] = reg_data[HM_SN_REG1] & 0xFF;            // low byte of reg 1
sn_bytes[3] = (reg_data[HM_SN_REG1 + 1] >> 8) & 0xFF; // high byte of reg 2
sn_bytes[4] = reg_data[HM_SN_REG1 + 1] & 0xFF;        // low byte of reg 2
sn_bytes[5] = (reg_data[HM_SN_PORT] >> 8) & 0xFF;     // high byte of reg 3

// Convert to hex string
for (int j = 0; j < 6; j++) {
    snprintf(&sn[j*2], 3, "%02x", sn_bytes[j]);
}
```

**Why this matters:** The serial number is byte-packed across registers, not aligned to register boundaries. The old code was reading from the wrong location.

---

### 4. Fixed Aggregation Function

**BEFORE (WRONG - tried to read Hoymiles data as SunSpec format):**
```cpp
uint16_t *r = s.raw_regs;
int16_t a_sf   = (int16_t)r[INV_A_SF];    // Wrong: these are SunSpec offsets
int16_t v_sf   = (int16_t)r[INV_V_SF];    // but raw_regs contains Hoymiles data!
float pw = apply_sf((int16_t)r[INV_W], w_sf);  // Reading garbage
```

**AFTER (CORRECT - use pre-decoded values):**
```cpp
// Use the already-decoded values from the Hoymiles data parsing
total_power += s.power_w;
total_current += s.current_a;
sum_freq += s.frequency_hz;
total_energy_wh += (uint32_t)(s.energy_kwh * 1000.0f);
// Phase distribution using s.voltage_v, s.current_a etc.
```

**Why this matters:** The aggregation function was trying to read `s.raw_regs` using SunSpec Model 101/103 register offsets (`INV_W`, `INV_A_SF`, etc.), but `raw_regs` actually contained Hoymiles-format data. This caused:
- Complete garbage in aggregated power/voltage/current values
- Phase distribution to be completely wrong
- Victron seeing nonsensical data

The fix uses the already-decoded float values (s.power_w, s.voltage_v, etc.) that were correctly parsed from the Hoymiles data.

---

### 5. Added DTU Diagnostics

**New features:**
- **DTU Serial Number Sensor** - Reads and displays the DTU-Pro's serial number (from register 0x2000)
- **DTU Online Status** - Binary sensor showing if DTU is responding (based on last successful poll within 30 seconds)
- **DTU Poll Success Counter** - Diagnostic sensor tracking successful DTU communications
- **DTU Poll Failure Counter** - Diagnostic sensor tracking failed DTU communications

**How it works:**
- On startup: reads DTU serial number from register 0x2000 (3 registers = 6-byte hex string)
- Every 60 seconds: re-polls DTU as an "alive check" to verify RS-485 link is working
- Published to Home Assistant as diagnostic sensors under the bridge

**Why this matters:** Previously, if inverters weren't responding, you couldn't tell if the problem was:
- RS-485 wiring issue
- DTU powered off/rebooted
- DTU Modbus address wrong
- Individual inverter offline

Now you can see DTU-level status separately from inverter status.

---

### 6. Enhanced Debug Logging

Added detailed hex register dumps (at VERBOSE level) to help troubleshoot future protocol issues:

```cpp
ESP_LOGD(TAG, "RTU RX: '%s' (port %d) — P=%.0fW (PV: %.1fV/%.2fA=%.0fW), "
         "Grid: %.0fV/%.2fHz, T=%.1f°C, Today=%.0fWh, Total=%.1fkWh, "
         "Status=0x%04X, Alarm=%d/%d, Link=0x%02X", ...);

ESP_LOGV(TAG, "  RAW regs 0-9: %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X", ...);
```

---

## Configuration Updates (`__init__.py`)

Added new DTU sensor options to `bridge_sensors` config:

```yaml
bridge_sensors:
  dtu_serial: true          # Text sensor: DTU serial number
  dtu_online: true          # Binary sensor: DTU responding
  dtu_poll_success: true    # Counter: successful DTU polls
  dtu_poll_fail: true       # Counter: failed DTU polls
```

All enabled by default for diagnostic visibility.

---

## Testing Recommendations

1. **Verify voltage readings** - Should be 230V ±10%, not 2300V or 23V
2. **Verify current readings** - Should be reasonable for inverter power (P/V), not 2× too high
3. **Verify frequency** - Should be 50Hz or 60Hz, not 5000Hz
4. **Verify serial numbers** - Should show hex strings in logs, not garbage
5. **Check DTU sensors** - Verify DTU serial appears in Home Assistant
6. **Check aggregated values** - Victron should see sensible total power/voltage/current
7. **Monitor alarm codes** - Should now show actual alarm data (was previously reading from wrong registers)

---

## Known Limitations

- **Link status register (0x10)** only uses high byte - low byte meaning unknown
- **3-phase inverters (HMT series)** - phase distribution is estimated (Hoymiles only provides total values, no per-phase breakdown for microinverters)
- **Alarm count register** - added but meaning not fully documented in Hoymiles spec
- **DTU polling** - happens every 60 seconds (hardcoded), could be made configurable if needed

---

## References

- **Hoymiles DTU-Pro-S Modbus Protocol** - Official Hoymiles specification (v1.x)
- **Python hoymiles-wifi library** - Used as reference for register layout verification
- **Node-RED Hoymiles parsers** - Confirmed byte offsets and scaling factors
- **SunSpec Model 101/103 spec** - For Victron GX device compatibility (output format)

---

## Migration Notes

**This is a breaking change** - inverter data values will change significantly after upgrading. You may need to:

1. **Recalibrate Home Assistant energy dashboards** - kWh totals should be preserved, but daily production graphs will show a discontinuity at the upgrade time
2. **Update automations** - Any automations based on voltage/current/power thresholds will need adjustment
3. **Check Victron GX settings** - ESS/DVCC power limits should be re-verified with corrected data

**Backup recommendation:** Take a snapshot of your ESPHome device config before upgrading in case you need to roll back.

---

## Future Improvements

Possible enhancements for future versions:

1. **Per-MPPT string monitoring** - Hoymiles may have additional registers for per-string voltage/current (not yet mapped)
2. **DTU configuration registers** - Some DTU settings may be writable (e.g., Wi-Fi config, inverter limits)
3. **HMS/HMT series differences** - Some models use different scaling (e.g., MIT series may use 0.1A instead of 0.01A for current)
4. **Alarm code decoding** - Map numeric alarm codes to human-readable descriptions
5. **Link status byte decoding** - Understand what the link status flags mean

---

## Credits

Fix implemented based on:
- Hoymiles official Modbus documentation
- Community reverse-engineering efforts (hoymiles-wifi, ahoy-dtu projects)
- Field testing with HMS-2000-4T inverters on DTU-Pro-S hardware

