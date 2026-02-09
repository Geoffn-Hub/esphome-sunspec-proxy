#pragma once

/**
 * Hoymiles Inverter Model Database
 * 
 * This file defines the known Hoymiles microinverter models and their specifications.
 * Use lookup_hoymiles_model() to get specs by model name.
 * 
 * Series Overview:
 * - HM-xxx: Legacy single-phase (2.4GHz RF)
 * - HMS-xxx-1T: Single-phase, 1 panel per inverter (300-500W)
 * - HMS-xxx-2T: Single-phase, 2 panels per inverter (600-1000W)
 * - HMS-xxxx-4T: Single-phase, 4 panels per inverter (1600-2000W)
 * - HMT-xxxx-4T: Three-phase, 4 panels per inverter (1600-2000W)
 * - HMT-xxxx-6T: Three-phase, 6 panels per inverter (2250W)
 * - MIT-xxxx-8T: Three-phase, 8 panels per inverter (4000-5000W)
 */

#include <cstdint>
#include <cstring>

namespace esphome {
namespace sunspec_proxy {

struct HoymilesModelSpec {
  const char* model_name;      // e.g., "HMS-2000-4T"
  uint16_t rated_power_w;      // Rated output power in watts
  uint8_t  mppt_inputs;        // Number of MPPT inputs (= number of DC inputs that are independently tracked)
  uint8_t  panel_inputs;       // Number of panel inputs (may differ from MPPT for 2T series)
  uint8_t  phases;             // 1 = single-phase, 3 = three-phase
  uint16_t max_vdc;            // Maximum DC input voltage
  uint16_t max_idc_per_input;  // Maximum DC current per input (×10, so 125 = 12.5A)
  uint16_t mppt_vmin;          // MPPT voltage range min
  uint16_t mppt_vmax;          // MPPT voltage range max
};

// Known Hoymiles inverter models
// Sources: hoymiles.com product pages, verified 2026-02
static const HoymilesModelSpec HOYMILES_MODELS[] = {
  // Legacy HM series (2.4GHz RF) - contributed by LoQue90
  {"HM-300",   300, 1, 1, 1, 60, 105, 22, 48},
  {"HM-350",   350, 1, 1, 1, 60, 105, 22, 48},
  {"HM-400",   400, 1, 1, 1, 60, 105, 22, 48},
  {"HM-600",   600, 1, 2, 1, 60, 115, 22, 48},
  {"HM-700",   700, 1, 2, 1, 60, 115, 22, 48},
  {"HM-800",   800, 1, 2, 1, 60, 115, 22, 48},
  {"HM-1200", 1200, 2, 4, 1, 60, 115, 22, 48},      // 2 MPPT, 4 panels
  {"HM-1500", 1500, 2, 4, 1, 60, 115, 22, 48},      // 2 MPPT, 4 panels

  // HMS Single-panel series (1T) - Single-phase
  {"HMS-300-1T",   300, 1, 1, 1, 60, 115, 16, 60},   // 11.5A
  {"HMS-350-1T",   350, 1, 1, 1, 60, 115, 16, 60},   // 11.5A
  {"HMS-400-1T",   400, 1, 1, 1, 65, 125, 16, 60},   // 12.5A
  {"HMS-450-1T",   450, 1, 1, 1, 65, 133, 16, 60},   // 13.3A
  {"HMS-500-1T",   500, 1, 1, 1, 65, 140, 16, 60},   // 14.0A

  // HMS Dual-panel series (2T) - Single-phase, shared MPPT
  {"HMS-600-2T",   600, 1, 2, 1, 60, 115, 16, 60},   // 2×11.5A, 1 MPPT for 2 panels
  {"HMS-700-2T",   700, 1, 2, 1, 60, 115, 16, 60},   // 2×11.5A
  {"HMS-800-2T",   800, 1, 2, 1, 65, 125, 16, 60},   // 2×12.5A
  {"HMS-900-2T",   900, 1, 2, 1, 65, 133, 16, 60},   // 2×13.3A
  {"HMS-1000-2T", 1000, 1, 2, 1, 65, 140, 16, 60},   // 2×14.0A

  // HMS Quad-panel series (4T) - Single-phase, 4 independent MPPTs
  {"HMS-1600-4T", 1600, 4, 4, 1, 65, 125, 16, 60},   // 4×12.5A
  {"HMS-1800-4T", 1800, 4, 4, 1, 65, 133, 16, 60},   // 4×13.3A
  {"HMS-2000-4T", 2000, 4, 4, 1, 65, 140, 16, 60},   // 4×14.0A

  // HMT Three-phase Quad-panel series (4T)
  {"HMT-1600-4T", 1600, 4, 4, 3, 65, 125, 16, 60},   // 4×12.5A, 3-phase
  {"HMT-1800-4T", 1800, 4, 4, 3, 65, 133, 16, 60},   // 4×13.3A, 3-phase
  {"HMT-2000-4T", 2000, 4, 4, 3, 65, 140, 16, 60},   // 4×14.0A, 3-phase

  // HMT Three-phase 6-panel series (6T)
  {"HMT-2250-6T", 2250, 3, 6, 3, 65, 140, 16, 60},   // 3 MPPT, 6 panels, 3-phase

  // MIT Three-phase 8-panel series (8T) - High-power commercial
  {"MIT-4000-8T", 4000, 4, 8, 3, 140, 200, 29, 120}, // 4×20A, 8 panels, 3-phase
  {"MIT-4500-8T", 4500, 4, 8, 3, 140, 200, 29, 120}, // 4×20A, 8 panels, 3-phase
  {"MIT-5000-8T", 5000, 4, 8, 3, 140, 200, 29, 120}, // 4×20A, 8 panels, 3-phase

  // Sentinel
  {nullptr, 0, 0, 0, 0, 0, 0, 0, 0}
};

static const size_t HOYMILES_MODEL_COUNT = sizeof(HOYMILES_MODELS) / sizeof(HOYMILES_MODELS[0]) - 1;

/**
 * Lookup a Hoymiles model by name (case-insensitive).
 * Returns nullptr if not found.
 */
inline const HoymilesModelSpec* lookup_hoymiles_model(const char* model_name) {
  if (model_name == nullptr) return nullptr;
  
  for (size_t i = 0; i < HOYMILES_MODEL_COUNT; i++) {
    if (strcasecmp(HOYMILES_MODELS[i].model_name, model_name) == 0) {
      return &HOYMILES_MODELS[i];
    }
  }
  return nullptr;
}

/**
 * Get the panel input count for a model. Returns the panel_inputs for DTU polling purposes,
 * as each panel input appears as a separate channel in DTU data regardless of MPPT count.
 */
inline uint8_t get_model_channel_count(const char* model_name) {
  const HoymilesModelSpec* spec = lookup_hoymiles_model(model_name);
  return spec ? spec->panel_inputs : 0;
}

/**
 * Get rated power for a model in watts.
 */
inline uint16_t get_model_rated_power(const char* model_name) {
  const HoymilesModelSpec* spec = lookup_hoymiles_model(model_name);
  return spec ? spec->rated_power_w : 0;
}

/**
 * Check if a model is three-phase.
 */
inline bool is_model_three_phase(const char* model_name) {
  const HoymilesModelSpec* spec = lookup_hoymiles_model(model_name);
  return spec ? (spec->phases == 3) : false;
}

}  // namespace sunspec_proxy
}  // namespace esphome
