# SunSpec Proxy for Hoymiles DTU-Pro

An ESPHome component that bridges Hoymiles microinverters to Victron energy systems via SunSpec over Modbus TCP.

## Overview

```
┌─────────────────┐    RS-485     ┌──────────────────┐    WiFi/TCP    ┌─────────────┐
│  Hoymiles DTU   │ ◄───────────► │  Waveshare ESP32 │ ◄────────────► │   Victron   │
│  (Port 0: HM1)  │   Modbus RTU  │  SunSpec Proxy   │   Modbus TCP   │   Cerbo GX  │
│  (Port 1: HM2)  │               │                  │                │             │
└─────────────────┘               └──────────────────┘                └─────────────┘
```

## Features

- **Polls Hoymiles DTU-Pro** via RS-485 Modbus RTU (Hoymiles register format)
- **Aggregates multiple inverters** (single-phase and 3-phase mix)
- **Presents SunSpec Model 103** (3-phase inverter) to Victron over Modbus TCP
- **Per-inverter sensors** for Home Assistant (power, energy, temperature, status)
- **Power limit forwarding** from Victron to inverters

## Supported Inverters

### HMS Series (Single-Phase)
- HMS-300/350/400/450/500-1T (1 MPPT)
- HMS-600/700/800/900/1000-2T (2 MPPT)
- HMS-1600/1800/2000-4T (4 MPPT)

### HMT Series (Three-Phase)
- HMT-1600/1800-4T (4 MPPT)

### MI/MIT Series
- MI-300/600/1200 (1/2/4 MPPT)
- MIT-3000/3200/3500-8T (8 MPPT, 3-phase)
- MIT-5000-8T (8 MPPT, 3-phase)

## Hardware Required

- **Waveshare ESP32-S3-ETH** (or similar ESP32 with RS-485)
- **Hoymiles DTU-Pro** with Modbus enabled
- **RS-485 cable** (A+ to A+, B- to B-)

## Configuration

```yaml
sunspec_proxy:
  id: sunspec_bridge
  uart_id: uart_bus
  tcp_port: 502
  dtu_address: 126          # DTU Modbus address (set in Hoymiles app)
  
  # Identity for Victron
  unit_id: 126
  phases: 3
  manufacturer: "Fronius"
  model_name: "Hoymiles Bridge"
  
  # Inverter ports on the DTU
  rtu_sources:
    - port: 0
      inverter_model: "HMS-2000-4T"
      name: "Garage Roof"
      connected_phase: 1    # Single-phase on L1
      
    - port: 1
      inverter_model: "MIT-5000-8T"
      name: "House Roof"
      # 3-phase uses all phases automatically
```

## Hoymiles Modbus Register Map

The DTU-Pro exposes inverter data at register 0x1000 + (port × 0x28):

| Offset | Description | Unit |
|--------|-------------|------|
| 0x08 | PV Voltage | V |
| 0x09 | PV Current | A × 2 |
| 0x0A | Grid Voltage | V |
| 0x0B | Grid Frequency | Hz × 100 |
| 0x0C | PV Power | W |
| 0x0D-0E | Today Production | Wh (32-bit) |
| 0x0F-10 | Total Production | Wh (32-bit) |
| 0x11 | Temperature | °C |
| 0x1E | Operating Status | - |
| 0x1F | Alarm Code | - |
| 0x20 | Link Status | - |

## Victron Setup

1. Add the ESP32's IP as a **Modbus TCP meter** in Victron
2. Select unit ID 126 (or your configured `unit_id`)
3. The device appears as a 3-phase PV inverter

## Building

```bash
cd /root/clawd/projects/sunspec-proxy
esphome compile waveshare-rs485.yaml
esphome upload waveshare-rs485.yaml --device 192.168.x.x
```

## References

- [Hoymiles DTU-Pro Modbus RTU Protocol v1.1](./docs/)
- [SunSpec Information Model](https://sunspec.org/)
- [ESPHome External Components](https://esphome.io/components/external_components.html)
