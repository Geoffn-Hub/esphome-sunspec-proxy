# Deployment Checklist ✅

## Pre-Deployment Verification

### ✅ Code Review
- [x] Register offsets verified against spec (byte address ÷ 2)
- [x] Scaling factors match spec's decimal column
- [x] Serial number extraction uses byte-packed format
- [x] Aggregation uses decoded float values (not raw_regs)
- [x] DTU polling implemented (serial + alive check)
- [x] Comments explain byte address conversion
- [x] Warning added about status register function codes

### ✅ Documentation
- [x] CHANGELOG-scaling-fix.md - Comprehensive change history
- [x] BYTE-ADDRESS-CLARIFICATION.md - Address conversion explained
- [x] IMPLEMENTATION-CONFIRMED.md - Verification that code is correct
- [x] FINAL-SUMMARY.md - Executive summary
- [x] QUICK-REFERENCE.md - Quick lookup tables
- [x] VERIFICATION.md - Testing checklist
- [x] DEPLOYMENT-CHECKLIST.md - This file

---

## Deployment Steps

### 1. Backup Current Configuration
```bash
# If using ESPHome dashboard
# - Download current .yaml config
# - Take screenshot of current sensor values (for comparison)
```

### 2. Compile Firmware
```bash
cd /root/clawd/projects/sunspec-proxy
esphome compile your-device.yaml
```

**Expected result:** Clean compilation with no errors.

**Common warnings (safe to ignore):**
- "Component sunspec_proxy took a long time for compilation"
- Standard ESPHome framework warnings

**Red flags (investigate):**
- Undefined constants (HM_*, check .h file)
- Type mismatch errors (check DTU field declarations)
- Missing function errors (check method signatures)

### 3. Flash Device
```bash
# OTA flash (if device is online)
esphome upload your-device.yaml

# OR Serial flash (if device is nearby)
esphome run your-device.yaml
```

### 4. Initial Boot Verification (First 5 minutes)

Watch the serial console or ESPHome logs:

**Set log level to VERBOSE temporarily:**
```yaml
logger:
  level: VERBOSE
  logs:
    sunspec_proxy: VERBOSE
```

**Look for:**
```
✅ [sunspec_proxy] SunSpec Proxy v1.2 — Hoymiles Modbus Mode
✅ [sunspec_proxy] DTU address: 126, 2 inverter ports
✅ [sunspec_proxy] Register map built: 178 registers, Model 103
✅ [sunspec_proxy] Modbus TCP listening on port 502 (unit_id=126)

✅ [sunspec_proxy] DTU: Reading serial number from address 0x2000
✅ [sunspec_proxy] DTU: Serial number: 99aabbccddee (poll OK count: 1)

✅ [sunspec_proxy] RTU TX: Reading port 0 from DTU 126 (regs 0x1000-0x1013)
✅ [sunspec_proxy] RTU RX: 'HMS-2000-4T-P0' (port 0) — P=1234W (PV: 123.4V/10.00A=1234W), ...
✅ [sunspec_proxy] AGG: P=2468W (L1:1234 L2:1234 L3:0) I=10.72A V=230.0/230.0/0.0V ...
```

**Red flags:**
```
❌ RTU: CRC error — Check RS-485 wiring/termination
❌ RTU: Timeout for ... — Check DTU address, baud rate
❌ RTU RX: Source 'XXX' short response — Wrong register count?
❌ Values look completely wrong — Register offset bug (report!)
```

### 5. Sensor Value Sanity Check

Open Home Assistant and verify these sensors appear with reasonable values:

#### Per-Inverter Sensors
- [ ] **Power:** 0-2000W (depends on sunlight and model rating)
- [ ] **Voltage:** 220-240V (or your local grid voltage)
- [ ] **Current:** 0-10A (reasonable for power level)
- [ ] **Frequency:** 49.5-50.5Hz (or 59.5-60.5Hz for 60Hz grids)
- [ ] **PV Voltage:** 30-60V (typical for 2-4 panels in series)
- [ ] **PV Current:** 0-15A (depends on panel rating)
- [ ] **PV Power:** Should approximately match AC power ±5%
- [ ] **Temperature:** 20-80°C (realistic for inverter)
- [ ] **Today Energy:** 0-50kWh (depends on time of day)
- [ ] **Total Energy:** Matches your known lifetime production
- [ ] **Alarm Code:** 0 (no alarms)
- [ ] **Alarm Count:** 0
- [ ] **Link Status:** 1 or 0x01 (connected)
- [ ] **Status:** "Producing" or "Idle" (not "Stale" or error)
- [ ] **Online:** ON (green)

#### Aggregate Sensors
- [ ] **Aggregate Power:** Sum of all inverters
- [ ] **Aggregate Voltage:** ~230V (average of all)
- [ ] **Aggregate Current:** Sum of all inverters
- [ ] **Aggregate Frequency:** ~50Hz (average)
- [ ] **Aggregate Energy:** Sum of all inverters

#### Bridge Sensors
- [ ] **TCP Clients:** 0 or 1 (Victron connected?)
- [ ] **TCP Requests:** Incrementing (if Victron polling)
- [ ] **TCP Errors:** 0 or very low
- [ ] **Victron Connected:** ON (if Victron GX active)
- [ ] **Power Limit:** 100% (unless limited)

#### DTU Sensors (NEW)
- [ ] **DTU Serial:** 12-char hex string (e.g., "99aabbccddee")
- [ ] **DTU Online:** ON (green)
- [ ] **DTU Poll Success:** >0 and incrementing
- [ ] **DTU Poll Failures:** 0 (or very low)

### 6. Compare Before/After Values

**Expected changes:**
- **Voltage:** If was showing 2300V or 23V → Now ~230V ✅
- **Current:** If was 2× too high → Now correct ✅
- **Frequency:** If was 5000Hz → Now 50Hz ✅
- **Temperature:** If was integer (45) → Now decimal (45.3°C) ✅
- **PV power:** If was way off → Now matches inverter output ✅
- **Total energy:** Should be roughly the same (kWh preserved) ✅

**Note:** There WILL be a discontinuity in your energy graphs at the upgrade time. This is normal and expected. The lifetime total should be correct.

### 7. Victron Integration Test (if applicable)

If you're using a Victron GX device:

1. **Check Victron portal or VRM:**
   - [ ] PV inverter device appears
   - [ ] Power reading is reasonable
   - [ ] AC-coupled PV power is detected

2. **Test ESS zero-feed:**
   - [ ] System responds to grid feed-in limits
   - [ ] Power limiting commands work (check logs)

3. **Check Modbus TCP:**
   - [ ] TCP Clients sensor shows 1 (Victron connected)
   - [ ] TCP Requests counter incrementing
   - [ ] No TCP errors accumulating

### 8. Run for 24 Hours

Monitor these over a full day:

- [ ] **Sunrise:** Power ramps up smoothly
- [ ] **Midday:** Peak power reasonable for your system
- [ ] **Sunset:** Power ramps down smoothly
- [ ] **Midnight:** Today Energy resets to 0
- [ ] **Total Energy:** Never decreases
- [ ] **No CRC errors** in logs over 24h
- [ ] **No timeouts** (or <1% failure rate)
- [ ] **DTU stays online** (poll success incrementing)
- [ ] **Sensors update** every 5 seconds

### 9. Performance Verification

After 24 hours, check:

```yaml
# Log these values from Home Assistant
Total energy before: _____ kWh
Total energy after:  _____ kWh
Difference: _____ kWh (should match actual production)

Peak power achieved: _____ W (should be ≤ system rating)
Max temperature: _____ °C (should be <85°C)

Poll success rate: _____ % (should be >95%)
Poll failure count: _____ (should be <50 per day)
CRC errors: _____ (should be 0)
Timeouts: _____ (should be <10 per day)
```

---

## Rollback Procedure (if needed)

If critical issues arise:

1. **Immediate:** Revert to previous ESPHome firmware
   ```bash
   # Flash backup .bin file
   esphome upload backup-firmware.bin
   ```

2. **Investigate:**
   - Check RS-485 wiring (A/B not swapped?)
   - Verify DTU Modbus address (default 126)
   - Check UART config (9600 8N1)
   - Verify termination resistors (120Ω at each end)

3. **Report:**
   - Save VERBOSE logs showing the issue
   - Note which values are wrong and by how much
   - Check if it's a hardware issue or code bug

**Note:** The register offsets and scaling are mathematically verified against the official Hoymiles spec, so code bugs are unlikely. Most issues will be configuration or hardware-related.

---

## Success Criteria

✅ Compilation succeeds without errors  
✅ Device boots and connects to WiFi  
✅ DTU serial appears in logs and Home Assistant  
✅ All inverter sensors show realistic values  
✅ Voltage ~230V (not 2300V or 23V)  
✅ Frequency ~50Hz (not 5000Hz)  
✅ Current = Power / Voltage (reasonable)  
✅ Temperature has decimal places  
✅ Serial numbers are 12-char hex strings  
✅ Aggregate values = sum of inverters  
✅ No CRC errors or excessive timeouts  
✅ Victron GX sees correct power (if connected)  
✅ System operates normally for 24 hours  

---

## Post-Deployment

### Update Documentation
- [ ] Note deployment date in project files
- [ ] Document any config tweaks needed
- [ ] Update energy dashboard baselines if needed

### Monitor Long-Term
- [ ] Check weekly for any new issues
- [ ] Verify energy totals match physical meter
- [ ] Monitor failure rates (should stay <1%)

### Optional: Restore Normal Logging
After verifying everything works, reduce log level:

```yaml
logger:
  level: INFO  # or DEBUG
  logs:
    sunspec_proxy: INFO  # Enough for normal operation
```

---

## Support Resources

**Documentation:**
- `QUICK-REFERENCE.md` - Quick lookup tables
- `BYTE-ADDRESS-CLARIFICATION.md` - Protocol details
- `VERIFICATION.md` - Testing guide

**Logs to collect for troubleshooting:**
```bash
# Enable VERBOSE logging first, then:
esphome logs your-device.yaml > debug.log

# Run for 5-10 minutes capturing a full poll cycle
# Include in support request if issues arise
```

**What to report if issues found:**
1. Exact values shown (e.g., "Voltage shows 2300V")
2. Expected values (e.g., "Should be 230V")
3. VERBOSE log excerpt showing the raw register data
4. Your inverter model (HMS-2000-4T, HMT-1600-6T, etc.)
5. DTU model (DTU-Pro, DTU-Pro-S, etc.)

---

## Final Checklist

- [ ] Code compiled without errors
- [ ] Firmware flashed successfully
- [ ] Device boots and logs look normal
- [ ] All sensors appear in Home Assistant
- [ ] Values pass sanity checks
- [ ] DTU diagnostics working
- [ ] Victron integration works (if applicable)
- [ ] 24-hour stability test passed
- [ ] Documentation updated
- [ ] Backup of working config saved

**Status:** ✅ READY TO DEPLOY

