/*
 *  File: header.c
 *
 *  Vektor - microKORG 2 oscillator unit header
 *  Names: max 8 characters on the microKORG 2.
 */

#include "unit.h"
#include "runtime.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api = UNIT_API_VERSION,
    .dev_id = 0x5477476EU,  // 'TwGn' (TwistedGenetics)
    .unit_id = 0x10U,  // 1 = VectorFM, 2 = Formant Mutator, 3 = Rollers, 4 = Dusty Pads, 5 = Horizon; 0x10 = Vektor (clear of IDs other new units may take)
    .version = 0x00010101U,  // 1.1.1
    .name = "Vektor",
    .num_presets = 0,
    .num_params = 11,
    .params = {
        // min, max, center, default, type, frac, frac mode, reserved, name

        // Page 1: the vector
        {0, 1023, 0, 512, k_unit_param_type_none, 0, 0, 0, {"VectorX"}},
        {0, 1023, 0, 512, k_unit_param_type_none, 0, 0, 0, {"VectorY"}},
        {0, 9, 0, 4, k_unit_param_type_strings, 0, 0, 0, {"Path"}},

        // Page 2: the sources
        {0, 15, 0, 2, k_unit_param_type_strings, 0, 0, 0, {"Wave A"}},
        {0, 15, 0, 8, k_unit_param_type_strings, 0, 0, 0, {"Wave B"}},
        {0, 15, 0, 7, k_unit_param_type_strings, 0, 0, 0, {"Wave C"}},
        {0, 15, 0, 6, k_unit_param_type_strings, 0, 0, 0, {"Wave D"}},
        {0, 100, 0, 20, k_unit_param_type_percent, 0, 0, 0, {"Detune"}},

        // Page 3: the path
        {0, 100, 0, 30, k_unit_param_type_none, 0, 0, 0, {"PathRate"}},
        {0, 100, 0, 70, k_unit_param_type_percent, 0, 0, 0, {"PathAmt"}},
        {0, 100, 0, 35, k_unit_param_type_none, 0, 0, 0, {"Bright"}}}};
