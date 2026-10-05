#pragma once

// Auto-generated from train_linear_clean.ipynb
static const float MP2_INTERCEPT = 539.366604327f;
static const float MP2_BASELINE = 89.000000000f;
static const float MP2_GATE_THRESHOLD = 400.000000000f;
static const float MP2_OUTPUT_EMA_TAU_S = 0.250000f;

static const int MP2_NUM_FEATURES = 7;

static const float MP2_COEFFICIENTS[MP2_NUM_FEATURES] = {
    0.005026791f,
    0.366429515f,
    -0.443136309f,
    -0.133795676f,
    -0.062876280f,
    0.029431210f,
    0.617684429f,
};

// Feature order:
// 0: IR_filtered
// 1: IR_ema_1s
// 2: IR_ema_5s
// 3: IR_ema_15s
// 4: IR_area_5s
// 5: IR_area_15s
// 6: IR_max_5s