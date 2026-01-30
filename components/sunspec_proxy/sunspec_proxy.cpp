#include "sunspec_proxy.h"
#include "esphome/core/log.h"
#include <cerrno>
#include <fcntl.h>
#include <cmath>

namespace esphome {
namespace sunspec_proxy {

static const char *const TAG = "sunspec_proxy";

// Helper: write string into uint16 register array (SunSpec string encoding: big-endian char pairs)
static void write_string_regs(uint16_t *regs, const char *str, int max_regs) {
  memset(regs, 0, max_regs * 2);
  int len = strlen(str);
  if (len > max_regs * 2) len = max_regs * 2;
  for (int i = 0; i < len; i++) {
    if (i % 2 == 0)
      regs[i / 2] = ((uint16_t)(uint8_t)str[i]) << 8;
    else
      regs[i / 2] |= (uint8_t)str[i];
  }
}

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

// Apply SunSpec scale factor to a raw int16 value
static float apply_sf(int16_t raw, int16_t sf) {
  if (raw == (int16_t)0x8000) return NAN; // not implemented
  return (float)raw * powf(10.0f, (float)sf);
}
static float apply_sf_u16(uint16_t raw, int16_t sf) {
  if (raw == 0xFFFF) return NAN; // not implemented
  return (float)raw * powf(10.0f, (float)sf);
}

// SunSpec operating state to human string
static const char *sunspec_state_str(uint16_t st) {
  switch (st) {
    case 1: return "Off";
    case 2: return "Sleeping";
    case 3: return "Starting";
    case 4: return "MPPT";
    case 5: return "Throttled";
    case 6: return "Shutting Down";
    case 7: return "Fault";
    case 8: return "Standby";
    default: return "Unknown";
  }
}

// ============================================================
// Configuration
// ============================================================

void SunSpecProxy::add_rtu_source(uint8_t port_number, uint8_t phases, uint16_t rated_power_w,
                                   uint8_t connected_phase, uint8_t mppt_inputs,
                                   const std::string &name, const std::string &model,
                                   const std::string &serial) {
  if (num_sources_ >= MAX_RTU_SOURCES) return;
  auto &s = sources_[num_sources_];
  memset(&s, 0, sizeof(RtuSource));
  s.port_number = port_number;
  s.phases = phases;
  s.connected_phase = (phases == 1) ? connected_phase : 0; // 0 = all phases (3-phase)
  s.rated_power_w = rated_power_w;
  s.mppt_inputs = mppt_inputs;
  strncpy(s.name, name.c_str(), 31); s.name[31] = 0;
  strncpy(s.model, model.c_str(), 23); s.model[23] = 0;
  strncpy(s.serial_number, serial.c_str(), 32); s.serial_number[32] = 0;
  s.data_valid = false;
  s.initial_model1_read = false;
  num_sources_++;
  if (phases == 1) {
    ESP_LOGI(TAG, "Added RTU source #%d: '%s' (%s) port=%d, 1-phase on L%d, %dW, %d MPPT",
             num_sources_ - 1, s.name, s.model, port_number, connected_phase, rated_power_w, mppt_inputs);
  } else {
    ESP_LOGI(TAG, "Added RTU source #%d: '%s' (%s) port=%d, 3-phase, %dW, %d MPPT",
             num_sources_ - 1, s.name, s.model, port_number, rated_power_w, mppt_inputs);
  }
  if (serial.length() > 0) {
    ESP_LOGI(TAG, "  Serial: %s", s.serial_number);
  }
}

// ============================================================
// Setup
// ============================================================

void SunSpecProxy::setup() {
  ESP_LOGI(TAG, "============================================");
  ESP_LOGI(TAG, "  SunSpec Proxy v1.2 — Hoymiles Modbus Mode");
  ESP_LOGI(TAG, "  DTU address: %d, %d inverter ports", dtu_address_, num_sources_);
  ESP_LOGI(TAG, "  Serving as unit_id %d on TCP :%d",
           agg_config_.unit_id, tcp_port_);
  ESP_LOGI(TAG, "  Manufacturer: %s", agg_config_.manufacturer);
  ESP_LOGI(TAG, "  Model: %s", agg_config_.model_name);
  ESP_LOGI(TAG, "  Serial: %s", agg_config_.serial_number);
  ESP_LOGI(TAG, "============================================");

  for (int i = 0; i < MAX_TCP_CLIENTS; i++) client_fds_[i] = -1;

  // Compute aggregated rated power/current from sources
  agg_config_.rated_power_w = 0;
  agg_config_.rated_current_a = 0;
  for (int i = 0; i < num_sources_; i++) {
    agg_config_.rated_power_w += sources_[i].rated_power_w;
    if (agg_config_.rated_voltage_v > 0) {
      agg_config_.rated_current_a += (float)sources_[i].rated_power_w / agg_config_.rated_voltage_v;
    }
    ESP_LOGI(TAG, "  Source #%d: '%s' port=%d, %dW",
             i, sources_[i].name, sources_[i].port_number, sources_[i].rated_power_w);
  }
  ESP_LOGI(TAG, "  Total rated: %dW, %.1fA @ %dV",
           agg_config_.rated_power_w, agg_config_.rated_current_a, agg_config_.rated_voltage_v);

  build_static_registers_();
  setup_tcp_server_();
}

void SunSpecProxy::loop() {
  handle_tcp_clients_();
  poll_rtu_sources_();

  // Periodic sensor publishing
  uint32_t now = millis();
  if (now - last_sensor_publish_ms_ >= SENSOR_PUBLISH_INTERVAL_MS) {
    last_sensor_publish_ms_ = now;
    for (int i = 0; i < num_sources_; i++) {
      publish_source_sensors_(i);
    }
    publish_aggregate_sensors_();
    publish_tcp_sensors_();
  }
}

// ============================================================
// Static Register Map Construction
// ============================================================

void SunSpecProxy::build_static_registers_() {
  for (int i = 0; i < TOTAL_REGS; i++) register_map_[i] = 0xFFFF;

  // --- SunS header ---
  register_map_[OFF_SUNS] = 0x5375;
  register_map_[OFF_SUNS + 1] = 0x6e53;

  // --- Model 1: Common Block ---
  register_map_[OFF_MODEL1] = 1;
  register_map_[OFF_MODEL1 + 1] = MODEL_1_SIZE;

  uint16_t *m1 = &register_map_[OFF_MODEL1 + 2];
  for (int i = 0; i < MODEL_1_SIZE; i++) m1[i] = 0x0000;
  write_string_regs(&m1[0], agg_config_.manufacturer, 16);
  write_string_regs(&m1[16], agg_config_.model_name, 16);
  write_string_regs(&m1[40], "1.1.0", 8);
  write_string_regs(&m1[48], agg_config_.serial_number, 16);
  m1[64] = agg_config_.unit_id;
  m1[65] = 0x8000;

  // --- Model 101/103: Inverter ---
  uint16_t model_id = (agg_config_.phases == 3) ? 103 : 101;
  register_map_[OFF_INV] = model_id;
  register_map_[OFF_INV + 1] = MODEL_103_SIZE;

  uint16_t *inv = &register_map_[OFF_INV + 2];
  for (int i = 0; i < MODEL_103_SIZE; i++) inv[i] = 0xFFFF;

  inv[INV_A_SF]   = (uint16_t)(int16_t)-2;
  inv[INV_V_SF]   = (uint16_t)(int16_t)-1;
  inv[INV_W_SF]   = (uint16_t)(int16_t) 0;
  inv[INV_Hz_SF]  = (uint16_t)(int16_t)-2;
  inv[INV_VA_SF]  = (uint16_t)(int16_t) 0;
  inv[INV_VAr_SF] = (uint16_t)(int16_t) 0;
  inv[INV_PF_SF]  = (uint16_t)(int16_t)-2;
  inv[INV_WH_SF]  = (uint16_t)(int16_t) 0;
  inv[INV_DCA_SF] = (uint16_t)(int16_t)-2;
  inv[INV_DCV_SF] = (uint16_t)(int16_t)-1;
  inv[INV_DCW_SF] = (uint16_t)(int16_t) 0;
  inv[INV_Tmp_SF] = (uint16_t)(int16_t)-1;

  inv[INV_St] = 2; // OFF
  inv[INV_Evt1] = 0; inv[INV_Evt1 + 1] = 0;
  inv[INV_Evt2] = 0; inv[INV_Evt2 + 1] = 0;
  inv[INV_EvtVnd1] = 0; inv[INV_EvtVnd1 + 1] = 0;
  inv[INV_EvtVnd2] = 0; inv[INV_EvtVnd2 + 1] = 0;
  inv[INV_EvtVnd3] = 0; inv[INV_EvtVnd3 + 1] = 0;
  inv[INV_EvtVnd4] = 0; inv[INV_EvtVnd4 + 1] = 0;

  // --- Model 120: Nameplate Ratings ---
  register_map_[OFF_M120] = 120;
  register_map_[OFF_M120 + 1] = MODEL_120_SIZE;
  uint16_t *m120 = &register_map_[OFF_M120 + 2];
  for (int i = 0; i < MODEL_120_SIZE; i++) m120[i] = 0xFFFF;
  m120[0] = 4;
  m120[1] = agg_config_.rated_power_w;
  m120[2] = 0;
  m120[3] = agg_config_.rated_power_w;
  m120[4] = 0;
  m120[10] = (uint16_t)(agg_config_.rated_current_a * 10);
  m120[11] = (uint16_t)(int16_t)-1;

  // --- Model 123: Immediate Controls ---
  register_map_[OFF_M123] = 123;
  register_map_[OFF_M123 + 1] = MODEL_123_SIZE;
  uint16_t *m123 = &register_map_[OFF_M123 + 2];
  for (int i = 0; i < MODEL_123_SIZE; i++) m123[i] = 0xFFFF;
  m123[2] = 1;                          // Conn = connected
  m123[3] = (uint16_t)(int16_t)-1;      // WMaxLimPct_SF
  m123[5] = 1000;                       // WMaxLimPct = 100.0%
  m123[8] = 0;                          // WMaxLim_Ena = disabled

  // --- End marker ---
  register_map_[OFF_END] = 0xFFFF;
  register_map_[OFF_END + 1] = 0;

  ESP_LOGI(TAG, "Register map built: %d registers, Model %d", TOTAL_REGS, model_id);
}

// ============================================================
// Aggregation
// ============================================================

void SunSpecProxy::aggregate_and_update_registers_() {
  uint16_t *inv = &register_map_[OFF_INV + 2];

  // Per-phase accumulators (real-world units)
  float phase_power[3] = {0, 0, 0};       // W per phase
  float phase_current[3] = {0, 0, 0};     // A per phase
  float phase_voltage_sum[3] = {0, 0, 0}; // V sum for averaging
  int phase_voltage_count[3] = {0, 0, 0};

  float total_power = 0, total_current = 0;
  float sum_freq = 0, total_va = 0, total_var = 0;
  uint32_t total_energy_wh = 0;
  float max_temp = NAN;
  float total_dc_power = 0;
  int valid_count = 0;
  bool any_producing = false;

  for (int i = 0; i < num_sources_; i++) {
    auto &s = sources_[i];
    if (!s.data_valid) continue;
    valid_count++;

    uint16_t *r = s.raw_regs;
    int16_t a_sf   = (int16_t)r[INV_A_SF];
    int16_t v_sf   = (int16_t)r[INV_V_SF];
    int16_t w_sf   = (int16_t)r[INV_W_SF];
    int16_t hz_sf  = (int16_t)r[INV_Hz_SF];
    int16_t va_sf  = (int16_t)r[INV_VA_SF];
    int16_t var_sf = (int16_t)r[INV_VAr_SF];
    int16_t wh_sf  = (int16_t)r[INV_WH_SF];
    int16_t dc_w_sf = (int16_t)r[INV_DCW_SF];
    int16_t tmp_sf = (int16_t)r[INV_Tmp_SF];

    // Total power from this source
    float pw = apply_sf((int16_t)r[INV_W], w_sf);
    if (!std::isnan(pw)) { total_power += pw; s.power_w = pw; if (pw > 0) any_producing = true; }
    else s.power_w = 0;

    // Total current from this source
    float cur = apply_sf_u16(r[INV_A], a_sf);
    if (!std::isnan(cur)) { total_current += cur; s.current_a = cur; } else s.current_a = 0;

    // Phase-aware current and voltage distribution
    if (s.phases == 3) {
      // 3-phase source: per-phase data comes directly from registers
      float i_a = apply_sf_u16(r[INV_AphA], a_sf);
      float i_b = apply_sf_u16(r[INV_AphB], a_sf);
      float i_c = apply_sf_u16(r[INV_AphC], a_sf);
      if (!std::isnan(i_a)) phase_current[0] += i_a;
      if (!std::isnan(i_b)) phase_current[1] += i_b;
      if (!std::isnan(i_c)) phase_current[2] += i_c;

      float v_a = apply_sf_u16(r[INV_PhVphA], v_sf);
      float v_b = apply_sf_u16(r[INV_PhVphB], v_sf);
      float v_c = apply_sf_u16(r[INV_PhVphC], v_sf);
      if (!std::isnan(v_a)) { phase_voltage_sum[0] += v_a; phase_voltage_count[0]++; }
      if (!std::isnan(v_b)) { phase_voltage_sum[1] += v_b; phase_voltage_count[1]++; }
      if (!std::isnan(v_c)) { phase_voltage_sum[2] += v_c; phase_voltage_count[2]++; }

      // Distribute power across phases (use per-phase current ratio if available)
      float total_i = (std::isnan(i_a) ? 0 : i_a) + (std::isnan(i_b) ? 0 : i_b) + (std::isnan(i_c) ? 0 : i_c);
      if (total_i > 0 && !std::isnan(pw)) {
        phase_power[0] += pw * (std::isnan(i_a) ? 0 : i_a) / total_i;
        phase_power[1] += pw * (std::isnan(i_b) ? 0 : i_b) / total_i;
        phase_power[2] += pw * (std::isnan(i_c) ? 0 : i_c) / total_i;
      } else if (!std::isnan(pw)) {
        // Equal split if no per-phase current
        phase_power[0] += pw / 3.0f;
        phase_power[1] += pw / 3.0f;
        phase_power[2] += pw / 3.0f;
      }

      s.voltage_v = std::isnan(v_a) ? 0 : v_a; // report phase A for the source sensor

    } else {
      // Single-phase source: all output goes to connected_phase
      int ph = s.connected_phase - 1; // 0-indexed (0=L1, 1=L2, 2=L3)
      if (ph < 0 || ph > 2) ph = 0;

      float i_a = apply_sf_u16(r[INV_AphA], a_sf);
      if (std::isnan(i_a)) i_a = cur; // fallback to total current
      if (!std::isnan(i_a)) phase_current[ph] += i_a;

      float v_a = apply_sf_u16(r[INV_PhVphA], v_sf);
      if (!std::isnan(v_a)) { phase_voltage_sum[ph] += v_a; phase_voltage_count[ph]++; s.voltage_v = v_a; }
      else s.voltage_v = 0;

      if (!std::isnan(pw)) phase_power[ph] += pw;
    }

    // VA / VAr (total, not per-phase)
    float va = apply_sf((int16_t)r[INV_VA], va_sf);
    if (!std::isnan(va)) total_va += va;
    float var_ = apply_sf((int16_t)r[INV_VAr], var_sf);
    if (!std::isnan(var_)) total_var += var_;

    // Frequency (averaged)
    float freq = apply_sf((int16_t)r[INV_Hz], hz_sf);
    if (!std::isnan(freq)) { sum_freq += freq; s.frequency_hz = freq; } else s.frequency_hz = 0;

    // Energy: acc32
    uint32_t e_raw = ((uint32_t)r[INV_WH] << 16) | r[INV_WH + 1];
    float e_mult = powf(10.0f, (float)wh_sf);
    uint32_t e_wh = (uint32_t)((float)e_raw * e_mult);
    total_energy_wh += e_wh;
    s.energy_kwh = (float)e_wh / 1000.0f;

    // Temperature
    float temp = apply_sf((int16_t)r[INV_TmpCab], tmp_sf);
    if (!std::isnan(temp)) {
      s.temperature_c = temp;
      if (std::isnan(max_temp) || temp > max_temp) max_temp = temp;
    }

    // DC power
    float dcp = apply_sf((int16_t)r[INV_DCW], dc_w_sf);
    if (!std::isnan(dcp)) total_dc_power += dcp;

    s.producing = (s.power_w > 0);
    update_source_status_(i);
  }

  if (valid_count == 0) {
    inv[INV_St] = 2;
    agg_power_w_ = 0; agg_current_a_ = 0; agg_voltage_v_ = 0; agg_frequency_hz_ = 0;
    ESP_LOGW(TAG, "Aggregation: no valid sources");
    return;
  }

  // Compute averaged voltages per phase
  float avg_v[3];
  for (int p = 0; p < 3; p++) {
    avg_v[p] = phase_voltage_count[p] > 0 ? phase_voltage_sum[p] / phase_voltage_count[p] : 0;
  }

  // Store aggregate decoded values
  agg_power_w_ = total_power;
  agg_current_a_ = total_current;
  agg_voltage_v_ = avg_v[0]; // report L1 as primary
  agg_frequency_hz_ = sum_freq / valid_count;
  agg_energy_kwh_ = (float)total_energy_wh / 1000.0f;

  // Write to register map
  // Our SFs: A=-2, V=-1, W=0, Hz=-2, VA=0, VAr=0, PF=-2, WH=0, Tmp=-1

  // Total AC power
  inv[INV_W] = (uint16_t)(int16_t)(int)total_power;

  // Total and per-phase current (SF=-2 → register = A * 100)
  inv[INV_A]    = (uint16_t)(total_current * 100.0f);
  inv[INV_AphA] = (uint16_t)(phase_current[0] * 100.0f);
  inv[INV_AphB] = (uint16_t)(phase_current[1] * 100.0f);
  inv[INV_AphC] = (uint16_t)(phase_current[2] * 100.0f);

  // Per-phase voltage (SF=-1 → register = V * 10)
  inv[INV_PhVphA] = (uint16_t)(avg_v[0] * 10.0f);
  inv[INV_PhVphB] = (uint16_t)(avg_v[1] * 10.0f);
  inv[INV_PhVphC] = (uint16_t)(avg_v[2] * 10.0f);

  // Line-to-line voltages (SF=-1)
  if (agg_config_.phases == 3) {
    // Proper L-L from L-N: Vab = sqrt(Va² + Vb² - 2*Va*Vb*cos(120°))
    // For balanced system: Vll ≈ Vln * sqrt(3)
    // Use actual phase voltages for better accuracy
    float vab = sqrtf(avg_v[0]*avg_v[0] + avg_v[1]*avg_v[1] + avg_v[0]*avg_v[1]); // cos(120°) = -0.5
    float vbc = sqrtf(avg_v[1]*avg_v[1] + avg_v[2]*avg_v[2] + avg_v[1]*avg_v[2]);
    float vca = sqrtf(avg_v[2]*avg_v[2] + avg_v[0]*avg_v[0] + avg_v[2]*avg_v[0]);
    inv[INV_PPVphAB] = (uint16_t)(vab * 10.0f);
    inv[INV_PPVphBC] = (uint16_t)(vbc * 10.0f);
    inv[INV_PPVphCA] = (uint16_t)(vca * 10.0f);
  }

  // Frequency (SF=-2 → Hz * 100)
  inv[INV_Hz] = (uint16_t)((sum_freq / valid_count) * 100.0f);

  // VA / VAr (SF=0)
  inv[INV_VA] = (uint16_t)(int16_t)(int)total_va;
  inv[INV_VAr] = (uint16_t)(int16_t)(int)total_var;

  // Power factor (SF=-2 → PF * 100)
  if (total_va > 0) {
    float pf = total_power / total_va;
    if (pf > 1.0f) pf = 1.0f;
    inv[INV_PF] = (uint16_t)(int16_t)(int)(pf * 100.0f);
  }

  // Energy (SF=0, acc32 Wh)
  inv[INV_WH]     = (uint16_t)(total_energy_wh >> 16);
  inv[INV_WH + 1] = (uint16_t)(total_energy_wh & 0xFFFF);

  // Temperature (SF=-1 → °C * 10)
  if (!std::isnan(max_temp)) {
    inv[INV_TmpCab] = (uint16_t)(int16_t)(int)(max_temp * 10.0f);
  }

  // DC power (SF=0)
  if (total_dc_power > 0) {
    inv[INV_DCW] = (uint16_t)(int16_t)(int)total_dc_power;
  }

  // Operating state
  inv[INV_St] = any_producing ? 4 : 2;

  ESP_LOGI(TAG, "AGG: P=%.0fW (L1:%.0f L2:%.0f L3:%.0f) I=%.2fA V=%.1f/%.1f/%.1fV f=%.2fHz E=%.1fkWh [%d/%d, %s]",
           total_power, phase_power[0], phase_power[1], phase_power[2],
           total_current, avg_v[0], avg_v[1], avg_v[2],
           sum_freq / valid_count,
           (float)total_energy_wh / 1000.0f,
           valid_count, num_sources_,
           any_producing ? "MPPT" : "Sleep");
}

// ============================================================
// Sensor Publishing
// ============================================================

void SunSpecProxy::update_source_status_(int idx) {
  auto &s = sources_[idx];
  if (!s.data_valid) return;

  uint32_t age_s = (millis() - s.last_poll_ms) / 1000;
  bool stale = age_s > (poll_interval_ms_ / 1000) * 3;

  if (src_status_sensors_[idx]) {
    char buf[64];
    if (stale) {
      snprintf(buf, sizeof(buf), "Stale (%lus)", age_s);
    } else if (s.producing) {
      snprintf(buf, sizeof(buf), "Producing %.0fW", s.power_w);
    } else {
      snprintf(buf, sizeof(buf), "Idle");
    }
    src_status_sensors_[idx]->publish_state(buf);
  }
}

void SunSpecProxy::publish_source_sensors_(int idx) {
  auto &s = sources_[idx];

  uint32_t age_s = s.data_valid ? (millis() - s.last_poll_ms) / 1000 : 999;
  bool online = s.data_valid && age_s < (poll_interval_ms_ / 1000) * 3;

  // Core electrical sensors
  if (src_power_sensors_[idx]) src_power_sensors_[idx]->publish_state(s.data_valid ? s.power_w : NAN);
  if (src_voltage_sensors_[idx]) src_voltage_sensors_[idx]->publish_state(s.data_valid ? s.voltage_v : NAN);
  if (src_current_sensors_[idx]) src_current_sensors_[idx]->publish_state(s.data_valid ? s.current_a : NAN);
  if (src_energy_sensors_[idx]) src_energy_sensors_[idx]->publish_state(s.data_valid ? s.energy_kwh : NAN);
  if (src_today_energy_sensors_[idx]) src_today_energy_sensors_[idx]->publish_state(s.data_valid ? s.today_energy_wh : NAN);
  if (src_frequency_sensors_[idx]) src_frequency_sensors_[idx]->publish_state(s.data_valid ? s.frequency_hz : NAN);
  if (src_temperature_sensors_[idx]) src_temperature_sensors_[idx]->publish_state(s.data_valid ? s.temperature_c : NAN);

  // DC (PV) side sensors
  if (src_pv_voltage_sensors_[idx]) src_pv_voltage_sensors_[idx]->publish_state(s.data_valid ? s.pv_voltage_v : NAN);
  if (src_pv_current_sensors_[idx]) src_pv_current_sensors_[idx]->publish_state(s.data_valid ? s.pv_current_a : NAN);
  if (src_pv_power_sensors_[idx]) src_pv_power_sensors_[idx]->publish_state(s.data_valid ? s.pv_power_w : NAN);

  // Status and diagnostics
  if (src_alarm_code_sensors_[idx]) src_alarm_code_sensors_[idx]->publish_state(s.data_valid ? s.alarm_code : 0);
  if (src_alarm_count_sensors_[idx]) src_alarm_count_sensors_[idx]->publish_state(s.data_valid ? s.alarm_count : 0);
  if (src_link_status_sensors_[idx]) src_link_status_sensors_[idx]->publish_state(s.data_valid ? s.link_status : 0);

  // Statistics
  if (src_poll_ok_sensors_[idx]) src_poll_ok_sensors_[idx]->publish_state(s.poll_success_count);
  if (src_poll_fail_sensors_[idx]) src_poll_fail_sensors_[idx]->publish_state(s.poll_fail_count + s.poll_timeout_count + s.crc_error_count);
  if (src_online_sensors_[idx]) src_online_sensors_[idx]->publish_state(online);

  update_source_status_(idx);
}

void SunSpecProxy::publish_aggregate_sensors_() {
  if (agg_power_sensor_) agg_power_sensor_->publish_state(agg_power_w_);
  if (agg_voltage_sensor_) agg_voltage_sensor_->publish_state(agg_voltage_v_);
  if (agg_current_sensor_) agg_current_sensor_->publish_state(agg_current_a_);
  if (agg_energy_sensor_) agg_energy_sensor_->publish_state(agg_energy_kwh_);
  if (agg_frequency_sensor_) agg_frequency_sensor_->publish_state(agg_frequency_hz_);
}

void SunSpecProxy::publish_tcp_sensors_() {
  // Count active TCP clients
  int active = 0;
  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    if (client_fds_[i] >= 0) active++;
  }

  bool victron_active = active > 0 && (millis() - last_tcp_activity_ms_ < 30000);

  if (tcp_clients_sensor_) tcp_clients_sensor_->publish_state(active);
  if (tcp_requests_sensor_) tcp_requests_sensor_->publish_state(tcp_request_count_);
  if (tcp_errors_sensor_) tcp_errors_sensor_->publish_state(tcp_error_count_);
  if (victron_connected_sensor_) victron_connected_sensor_->publish_state(victron_active);

  if (victron_status_sensor_) {
    if (!victron_active && active == 0) {
      victron_status_sensor_->publish_state("No connection");
    } else if (!victron_active) {
      victron_status_sensor_->publish_state("Connected, idle");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "Active (%lu reqs)", tcp_request_count_);
      victron_status_sensor_->publish_state(buf);
    }
  }

  // Power limit
  if (power_limit_sensor_) {
    uint16_t pct = register_map_[OFF_M123 + 2 + 5]; // WMaxLimPct
    uint16_t ena = register_map_[OFF_M123 + 2 + 8]; // WMaxLim_Ena
    power_limit_sensor_->publish_state(ena == 1 ? pct / 10.0f : 100.0f);
  }
}

// ============================================================
// TCP Server
// ============================================================

void SunSpecProxy::setup_tcp_server_() {
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
    return;
  }

  int opt = 1;
  setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  fcntl(server_fd_, F_SETFL, fcntl(server_fd_, F_GETFL, 0) | O_NONBLOCK);

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(tcp_port_);

  if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "TCP bind port %d failed: errno=%d", tcp_port_, errno);
    close(server_fd_); server_fd_ = -1; return;
  }
  if (listen(server_fd_, 4) < 0) {
    ESP_LOGE(TAG, "TCP listen failed: errno=%d", errno);
    close(server_fd_); server_fd_ = -1; return;
  }

  ESP_LOGI(TAG, "Modbus TCP listening on port %d (unit_id=%d)", tcp_port_, agg_config_.unit_id);
}

void SunSpecProxy::handle_tcp_clients_() {
  if (server_fd_ < 0) return;

  struct sockaddr_in ca;
  socklen_t al = sizeof(ca);
  int nfd = accept(server_fd_, (struct sockaddr *)&ca, &al);
  if (nfd >= 0) {
    fcntl(nfd, F_SETFL, fcntl(nfd, F_GETFL, 0) | O_NONBLOCK);
    bool placed = false;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
      if (client_fds_[i] < 0) {
        client_fds_[i] = nfd;
        placed = true;
        char ip[16];
        inet_ntoa_r(ca.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "TCP: Client connected from %s (slot %d)", ip, i);
        break;
      }
    }
    if (!placed) {
      ESP_LOGW(TAG, "TCP: No slot available, rejecting connection");
      close(nfd);
    }
  }

  uint8_t buf[260];
  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    if (client_fds_[i] < 0) continue;
    int n = recv(client_fds_[i], buf, sizeof(buf), 0);
    if (n > 0) {
      process_tcp_request_(client_fds_[i], buf, n);
    } else if (n == 0) {
      ESP_LOGI(TAG, "TCP: Client slot %d disconnected", i);
      close(client_fds_[i]); client_fds_[i] = -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "TCP: Client slot %d error: errno=%d", i, errno);
      close(client_fds_[i]); client_fds_[i] = -1;
    }
  }
}

void SunSpecProxy::process_tcp_request_(int client_fd, uint8_t *buf, int len) {
  if (len < 8) return;

  uint16_t txn_id = be16(&buf[0]);
  uint16_t proto = be16(&buf[2]);
  uint8_t unit_id = buf[6];
  uint8_t fc = buf[7];

  if (proto != 0) return;

  last_tcp_activity_ms_ = millis();
  tcp_request_count_++;

  if (unit_id != agg_config_.unit_id) {
    ESP_LOGD(TAG, "TCP: Request for unit_id %d (we are %d), ignoring", unit_id, agg_config_.unit_id);
    return;
  }

  switch (fc) {
    case 0x03: { // Read Holding Registers
      if (len < 12) return;
      uint16_t start = be16(&buf[8]);
      uint16_t count = be16(&buf[10]);

      ESP_LOGD(TAG, "TCP RX: ReadHolding txn=%d unit=%d reg=%d count=%d",
               txn_id, unit_id, start, count);

      if (count > 125) {
        ESP_LOGW(TAG, "TCP: Read count %d exceeds max 125", count);
        send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x03);
        tcp_error_count_++;
        return;
      }

      uint16_t values[125];
      if (!read_sunspec_registers_(start, count, values)) {
        ESP_LOGW(TAG, "TCP: Read failed for reg %d count %d (out of range)", start, count);
        send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x02);
        tcp_error_count_++;
        return;
      }

      uint8_t resp[1 + 250];
      resp[0] = count * 2;
      for (int i = 0; i < count; i++) put_be16(&resp[1 + i * 2], values[i]);
      send_tcp_response_(client_fd, txn_id, unit_id, fc, resp, 1 + count * 2);

      ESP_LOGV(TAG, "TCP TX: ReadHolding response %d regs", count);
      break;
    }
    case 0x06: { // Write Single Register
      if (len < 12) return;
      uint16_t reg = be16(&buf[8]);
      uint16_t val = be16(&buf[10]);

      ESP_LOGI(TAG, "TCP RX: WriteSingle txn=%d reg=%d val=%d", txn_id, reg, val);

      if (!write_sunspec_registers_(reg, 1, &val)) {
        send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x02);
        tcp_error_count_++;
        return;
      }
      uint8_t resp[4]; put_be16(&resp[0], reg); put_be16(&resp[2], val);
      send_tcp_response_(client_fd, txn_id, unit_id, fc, resp, 4);
      break;
    }
    case 0x10: { // Write Multiple Registers
      if (len < 13) return;
      uint16_t reg = be16(&buf[8]);
      uint16_t cnt = be16(&buf[10]);

      ESP_LOGI(TAG, "TCP RX: WriteMultiple txn=%d reg=%d count=%d", txn_id, reg, cnt);

      if (len < 13 + cnt * 2 || cnt > 100) {
        send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x03);
        tcp_error_count_++;
        return;
      }
      uint16_t vals[100];
      for (int i = 0; i < cnt; i++) vals[i] = be16(&buf[13 + i * 2]);
      if (!write_sunspec_registers_(reg, cnt, vals)) {
        send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x02);
        tcp_error_count_++;
        return;
      }
      uint8_t resp[4]; put_be16(&resp[0], reg); put_be16(&resp[2], cnt);
      send_tcp_response_(client_fd, txn_id, unit_id, fc, resp, 4);
      break;
    }
    default:
      ESP_LOGW(TAG, "TCP: Unsupported function code 0x%02X", fc);
      send_tcp_error_(client_fd, txn_id, unit_id, fc, 0x01);
      tcp_error_count_++;
  }
}

void SunSpecProxy::send_tcp_response_(int client_fd, uint16_t txn_id, uint8_t unit_id,
                                       uint8_t fc, const uint8_t *data, uint16_t data_len) {
  uint8_t frame[260];
  put_be16(&frame[0], txn_id);
  put_be16(&frame[2], 0);
  put_be16(&frame[4], 1 + 1 + data_len);
  frame[6] = unit_id;
  frame[7] = fc;
  memcpy(&frame[8], data, data_len);
  send(client_fd, frame, 8 + data_len, 0);
}

void SunSpecProxy::send_tcp_error_(int client_fd, uint16_t txn_id, uint8_t unit_id,
                                    uint8_t fc, uint8_t err) {
  uint8_t data[1] = {err};
  send_tcp_response_(client_fd, txn_id, unit_id, fc | 0x80, data, 1);
}

// ============================================================
// SunSpec Register Access
// ============================================================

bool SunSpecProxy::read_sunspec_registers_(uint16_t start_reg, uint16_t count, uint16_t *out) {
  if (start_reg < SUNSPEC_BASE) return false;
  uint16_t off = start_reg - SUNSPEC_BASE;
  if (off + count > TOTAL_REGS) return false;
  memcpy(out, &register_map_[off], count * 2);
  return true;
}

bool SunSpecProxy::write_sunspec_registers_(uint16_t start_reg, uint16_t count, const uint16_t *values) {
  if (start_reg < SUNSPEC_BASE) return false;
  uint16_t off = start_reg - SUNSPEC_BASE;

  if (off < OFF_M123 + 2 || off + count > OFF_END) {
    ESP_LOGW(TAG, "TCP: Write rejected — register %d not in Model 123 writable area", start_reg);
    return false;
  }

  for (uint16_t i = 0; i < count; i++) register_map_[off + i] = values[i];

  uint16_t lim_off = OFF_M123 + 2 + 5;
  uint16_t ena_off = OFF_M123 + 2 + 8;
  bool changed = false;
  for (uint16_t r = off; r < off + count; r++) {
    if (r == lim_off || r == ena_off) { changed = true; break; }
  }

  if (changed) {
    uint16_t pct = register_map_[lim_off];
    uint16_t ena = register_map_[ena_off];
    ESP_LOGI(TAG, "VICTRON: Power limit command — %.1f%%, enabled=%d", pct / 10.0f, ena);
    forward_power_limit_(pct, ena == 1);
  }

  return true;
}

// ============================================================
// Power Limit Forwarding
// ============================================================

void SunSpecProxy::forward_power_limit_(uint16_t pct_raw, bool enabled) {
  // Hoymiles status register layout per port:
  // Port 0: 0xC006=ON/OFF, 0xC007=Limit%
  // Port 1: 0xC00C=ON/OFF, 0xC00D=Limit%
  // Port N: 0xC006 + N*6 = ON/OFF, 0xC007 + N*6 = Limit%
  
  for (int i = 0; i < num_sources_; i++) {
    uint8_t port = sources_[i].port_number;
    uint16_t onoff_reg = 0xC006 + (port * 6);
    uint16_t limit_reg = 0xC007 + (port * 6);

    if (enabled) {
      // Convert SunSpec percentage (0-1000 = 0-100.0%) to Hoymiles percentage (2-100)
      int hm_pct = pct_raw / 10;
      if (hm_pct < 2) hm_pct = 2;
      if (hm_pct > 100) hm_pct = 100;

      ESP_LOGI(TAG, "RTU TX: Power limit %d%% to '%s' (DTU %d, port %d, reg 0x%04X)",
               hm_pct, sources_[i].name, dtu_address_, port, limit_reg);

      // Write power limit percentage
      uint8_t f1[8];
      f1[0] = dtu_address_; f1[1] = 0x06;
      put_be16(&f1[2], limit_reg);
      put_be16(&f1[4], hm_pct);
      uint16_t crc = calc_crc16_(f1, 6);
      f1[6] = crc & 0xFF; f1[7] = crc >> 8;
      this->write_array(f1, 8);
      this->flush();
      delay(100);

      // Ensure inverter is ON
      uint8_t f2[8];
      f2[0] = dtu_address_; f2[1] = 0x05;  // Function 0x05 for single coil write
      put_be16(&f2[2], onoff_reg);
      put_be16(&f2[4], 0xFF00);  // 0xFF00 = ON
      uint16_t crc2 = calc_crc16_(f2, 6);
      f2[6] = crc2 & 0xFF; f2[7] = crc2 >> 8;
      this->write_array(f2, 8);
      this->flush();
      delay(100);
    } else {
      ESP_LOGI(TAG, "RTU TX: Removing power limit on '%s' (DTU %d, port %d)",
               sources_[i].name, dtu_address_, port);

      // Set limit to 100%
      uint8_t f[8];
      f[0] = dtu_address_; f[1] = 0x06;
      put_be16(&f[2], limit_reg);
      put_be16(&f[4], 100);
      uint16_t crc = calc_crc16_(f, 6);
      f[6] = crc & 0xFF; f[7] = crc >> 8;
      this->write_array(f, 8);
      this->flush();
      delay(100);
    }
  }
}

// ============================================================
// RTU Polling (Hoymiles Modbus RTU Protocol)
// ============================================================
// 
// The DTU-Pro is polled at dtu_address_ using Hoymiles Modbus registers:
// - Register 0x1000 + (port * 0x28) = Data for inverter at that port
// - Each port block is 40 registers (0x28 = 40 decimal)
// 
// Register layout per port (offset from port base):
//   0x00: Data type
//   0x01-0x06: Serial number (6 regs = 12 chars)
//   0x07: Port number
//   0x08: PV Voltage (V)
//   0x09: PV Current (A * 2)
//   0x0A: Grid Voltage (V)
//   0x0B: Grid Frequency (Hz * 100)
//   0x0C: PV Power (W)
//   0x0D-0x0E: Today Production (Wh, 32-bit)
//   0x0F-0x10: Total Production (Wh, 32-bit)
//   0x11: Temperature (°C)
//   0x1E: Operating Status
//   0x1F: Alarm Code
//   0x20: Link Status

void SunSpecProxy::poll_rtu_sources_() {
  if (num_sources_ == 0) return;
  uint32_t now = millis();

  // Handle pending RTU response
  if (rtu_busy_) {
    uint8_t resp[256];
    int n = read_rtu_response_(resp, sizeof(resp));

    if (n > 0) {
      auto &s = sources_[current_poll_source_];

      if (n >= 5 && resp[1] == 0x03) {
        uint8_t byte_count = resp[2];
        int reg_count = byte_count / 2;
        uint16_t reg_data[64];
        for (int i = 0; i < reg_count && i < 64; i++) {
          reg_data[i] = be16(&resp[3 + i * 2]);
        }

        // Parse Hoymiles data registers
        if (reg_count >= 34) {  // Need at least up to 0x21 (34 regs)
          // Extract serial number from regs 0x01-0x06 (6 regs = 12 digits)
          if (!s.initial_model1_read) {
            char sn[13] = {0};
            for (int i = 0; i < 6 && (1 + i) < reg_count; i++) {
              sn[i * 2]     = (reg_data[1 + i] >> 8) & 0xFF;
              sn[i * 2 + 1] = reg_data[1 + i] & 0xFF;
            }
            sn[12] = 0;
            // Trim trailing nulls/spaces
            for (int i = 11; i >= 0; i--) {
              if (sn[i] == 0 || sn[i] == ' ') sn[i] = 0; else break;
            }
            if (sn[0] != 0) {
              strncpy(s.serial_from_dtu, sn, 32);
              if (s.serial_number[0] == 0) {
                strncpy(s.serial_number, sn, 32);
              }
              ESP_LOGI(TAG, "RTU RX: Source '%s' (port %d) serial: %s", s.name, s.port_number, sn);
            }
            s.initial_model1_read = true;
          }

          // Parse live data
          s.pv_voltage_v = (float)reg_data[HM_PV_VOLTAGE];
          s.pv_current_a = (float)reg_data[HM_PV_CURRENT] / 2.0f;  // Scaled by 2
          s.voltage_v = (float)reg_data[HM_GRID_VOLTAGE];
          s.frequency_hz = (float)reg_data[HM_GRID_FREQ] / 100.0f;  // Scaled by 100
          s.pv_power_w = (float)reg_data[HM_PV_POWER];
          s.power_w = s.pv_power_w;  // For AC output, use PV power (micro-inverter)
          
          // 32-bit values (high word first in Hoymiles format)
          uint32_t today_wh = ((uint32_t)reg_data[HM_TODAY_PROD_H] << 16) | reg_data[HM_TODAY_PROD_L];
          uint32_t total_wh = ((uint32_t)reg_data[HM_TOTAL_PROD_H] << 16) | reg_data[HM_TOTAL_PROD_L];
          s.today_energy_wh = (float)today_wh;
          s.energy_kwh = (float)total_wh / 1000.0f;
          
          s.temperature_c = (float)(int16_t)reg_data[HM_TEMPERATURE];
          s.operating_status = reg_data[HM_OPERATING_STATUS];
          s.alarm_code = reg_data[HM_ALARM_CODE];
          s.link_status = reg_data[HM_LINK_STATUS] & 0xFF;
          s.producing = (s.power_w > 0);

          // Estimate AC current from power/voltage
          if (s.voltage_v > 0) {
            s.current_a = s.power_w / s.voltage_v;
          }

          s.data_valid = true;
          s.last_poll_ms = now;
          s.poll_success_count++;

          ESP_LOGD(TAG, "RTU RX: '%s' (port %d) — P=%.0fW, V=%.0fV, I=%.2fA, F=%.2fHz, T=%.0f°C, E=%.1fkWh",
                   s.name, s.port_number, s.power_w, s.voltage_v, s.current_a, 
                   s.frequency_hz, s.temperature_c, s.energy_kwh);

          aggregate_and_update_registers_();
        } else {
          ESP_LOGW(TAG, "RTU RX: Source '%s' short response: %d regs (need 34)",
                   s.name, reg_count);
          s.poll_fail_count++;
        }
      } else if (n >= 2 && (resp[1] & 0x80)) {
        // Exception response
        uint8_t exc = n >= 3 ? resp[2] : 0;
        ESP_LOGW(TAG, "RTU RX: DTU exception for port %d: func=0x%02X exc=%d",
                 sources_[current_poll_source_].port_number, resp[1], exc);
        sources_[current_poll_source_].poll_fail_count++;
      } else {
        ESP_LOGW(TAG, "RTU RX: Unexpected response (%d bytes, func=0x%02X)",
                 n, resp[1]);
        sources_[current_poll_source_].poll_fail_count++;
      }

      rtu_busy_ = false;

    } else if (n == -1) {
      // CRC error (already logged in read_rtu_response_)
      sources_[current_poll_source_].crc_error_count++;
      sources_[current_poll_source_].poll_fail_count++;
      rtu_busy_ = false;

    } else if (now - rtu_request_time_ > rtu_timeout_ms_) {
      auto &s = sources_[current_poll_source_];
      s.poll_timeout_count++;
      ESP_LOGW(TAG, "RTU: Timeout for '%s' (DTU %d, port %d) — timeouts=%lu",
               s.name, dtu_address_, s.port_number, s.poll_timeout_count);
      rtu_busy_ = false;
    }
    return;
  }

  // Time for next poll?
  uint32_t interval_per_source = poll_interval_ms_ / num_sources_;
  if (now - last_poll_time_ < interval_per_source) return;

  auto &s = sources_[current_poll_source_];

  // Calculate register address for this port: 0x1000 + (port * 0x28)
  uint16_t port_base = HM_DATA_BASE + (s.port_number * HM_PORT_STRIDE);
  
  ESP_LOGV(TAG, "RTU TX: Reading port %d from DTU %d (regs 0x%04X-0x%04X)",
           s.port_number, dtu_address_, port_base, port_base + HM_PORT_REGS - 1);
  
  // Read all 40 registers for this port from the DTU
  if (send_rtu_request_(dtu_address_, 0x03, port_base, HM_PORT_REGS)) {
    rtu_busy_ = true;
    rtu_request_time_ = now;
  }

  // Move to next source
  current_poll_source_ = (current_poll_source_ + 1) % num_sources_;
  last_poll_time_ = now;
}

bool SunSpecProxy::send_rtu_request_(uint8_t address, uint8_t function, uint16_t reg_start, uint16_t reg_count) {
  while (this->available()) this->read();

  uint8_t frame[8];
  frame[0] = address;
  frame[1] = function;
  put_be16(&frame[2], reg_start);
  put_be16(&frame[4], reg_count);
  uint16_t crc = calc_crc16_(frame, 6);
  frame[6] = crc & 0xFF;
  frame[7] = (crc >> 8) & 0xFF;

  this->write_array(frame, 8);
  this->flush();
  return true;
}

int SunSpecProxy::read_rtu_response_(uint8_t *buf, int max_len) {
  int n = 0;
  while (this->available() && n < max_len) {
    buf[n++] = this->read();
  }
  if (n < 5) return 0;

  uint16_t expected = calc_crc16_(buf, n - 2);
  uint16_t received = buf[n - 2] | ((uint16_t)buf[n - 1] << 8);
  if (expected != received) {
    ESP_LOGW(TAG, "RTU: CRC error — expected 0x%04X, got 0x%04X (%d bytes from addr %d)",
             expected, received, n, buf[0]);
    return -1;
  }
  return n;
}

uint16_t SunSpecProxy::calc_crc16_(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

}  // namespace sunspec_proxy
}  // namespace esphome
