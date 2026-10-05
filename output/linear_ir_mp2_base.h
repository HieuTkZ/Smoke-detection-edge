#pragma once

/*
 * ================================================================
 *  IR -> MP2 BASE MODEL FOR ESP32
 * ================================================================
 *
 *  Auto-generated from final_base_with_esp32_header_lowlag.ipynb
 *
 *  Required update rate:
 *      20 Hz  ->  one call every 50 ms
 *
 *  Pipeline:
 *
 *      IR_raw
 *        -> IR baseline correction
 *        -> causal EMA filter (tau = 0.25 s)
 *        -> 7 temporal IR features
 *        -> Quantile Regression (q = 0.38)
 *        -> baseline gate
 *        -> output EMA (tau = 0.250 s)
 *        -> Model Pred
 *
 *  IMPORTANT:
 *  - MP2_DO is not used.
 *  - irBaseline must be measured in clean air before prediction.
 *  - The feature order and formulas must remain unchanged unless
 *    the Python training pipeline is changed and this file is
 *    generated again.
 * ================================================================
 */

#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------
// Sampling configuration
// ---------------------------------------------------------------
static const float MP2_MODEL_FS_HZ = 20.000000f;
static const float MP2_MODEL_DT_S  = 0.050000000f;

static const int MP2_AREA_5S_SAMPLES   = 100;
static const int MP2_AREA_15S_SAMPLES  = 300;
static const int MP2_MAX_5S_SAMPLES    = 100;
static const int MP2_HISTORY_SIZE      = 300;

// ---------------------------------------------------------------
// Model constants extracted from Python
// ---------------------------------------------------------------
static const float MP2_INTERCEPT =
    539.366604327f;

static const float MP2_BASELINE =
    89.000000000f;

static const float MP2_GATE_THRESHOLD =
    400.000000000f;

static const float MP2_OUTPUT_EMA_TAU_S =
    0.250000000f;

// ---------------------------------------------------------------
// EMA coefficients for dt = 0.05 s
// alpha = 1 - exp(-dt / tau)
// ---------------------------------------------------------------
static const float MP2_ALPHA_IR_FILTER =
    0.181269247f;

static const float MP2_ALPHA_EMA_1S =
    0.048770575f;

static const float MP2_ALPHA_EMA_5S =
    0.009950166f;

static const float MP2_ALPHA_EMA_15S =
    0.003327784f;

static const float MP2_ALPHA_OUTPUT =
    0.181269247f;

// ---------------------------------------------------------------
// Feature order used during training
//
// 0: IR_filtered
// 1: IR_ema_1s
// 2: IR_ema_5s
// 3: IR_ema_15s
// 4: IR_area_5s
// 5: IR_area_15s
// 6: IR_max_5s
// ---------------------------------------------------------------
static const int MP2_NUM_FEATURES = 7;

static const float MP2_COEFFICIENTS[MP2_NUM_FEATURES] = {
    0.005026791f,
    0.366429515f,
    -0.443136309f,
    -0.133795676f,
    -0.062876280f,
    0.029431210f,
    0.617684429f
};

// ---------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------
struct MP2ModelState {
    float irBaseline;

    float irFiltered;
    float irEma1s;
    float irEma5s;
    float irEma15s;

    float history[MP2_HISTORY_SIZE];
    int historyIndex;
    int historyCount;

    float modelPred;

    bool initialized;
};

// ---------------------------------------------------------------
// Utility
// ---------------------------------------------------------------
static inline float mp2Clamp(
    float value,
    float low,
    float high
) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// ---------------------------------------------------------------
// Reset model state.
//
// irBaseline:
// Median/representative IR_raw value measured in clean air.
// This corresponds to IR baseline correction in Python.
// ---------------------------------------------------------------
static inline void mp2ModelReset(
    MP2ModelState &state,
    float irBaseline
) {
    state.irBaseline = irBaseline;

    state.irFiltered = 0.0f;
    state.irEma1s = 0.0f;
    state.irEma5s = 0.0f;
    state.irEma15s = 0.0f;

    state.historyIndex = 0;
    state.historyCount = 0;

    state.modelPred = MP2_BASELINE;
    state.initialized = false;

    for (int i = 0; i < MP2_HISTORY_SIZE; ++i) {
        state.history[i] = 0.0f;
    }
}

// ---------------------------------------------------------------
// Add current IR_filtered sample to history.
// ---------------------------------------------------------------
static inline void mp2PushHistory(
    MP2ModelState &state,
    float value
) {
    state.history[state.historyIndex] = value;

    state.historyIndex++;
    if (state.historyIndex >= MP2_HISTORY_SIZE) {
        state.historyIndex = 0;
    }

    if (state.historyCount < MP2_HISTORY_SIZE) {
        state.historyCount++;
    }
}

// ---------------------------------------------------------------
// Sum the N most recent IR_filtered samples.
// ---------------------------------------------------------------
static inline float mp2RecentSum(
    const MP2ModelState &state,
    int requestedSamples
) {
    int count = state.historyCount;

    if (count > requestedSamples) {
        count = requestedSamples;
    }

    float sum = 0.0f;

    for (int i = 0; i < count; ++i) {
        int index =
            state.historyIndex - 1 - i;

        if (index < 0) {
            index += MP2_HISTORY_SIZE;
        }

        sum += state.history[index];
    }

    return sum;
}

// ---------------------------------------------------------------
// Max of the N most recent IR_filtered samples.
// ---------------------------------------------------------------
static inline float mp2RecentMax(
    const MP2ModelState &state,
    int requestedSamples
) {
    int count = state.historyCount;

    if (count > requestedSamples) {
        count = requestedSamples;
    }

    if (count <= 0) {
        return 0.0f;
    }

    float maxValue = 0.0f;

    for (int i = 0; i < count; ++i) {
        int index =
            state.historyIndex - 1 - i;

        if (index < 0) {
            index += MP2_HISTORY_SIZE;
        }

        if (state.history[index] > maxValue) {
            maxValue = state.history[index];
        }
    }

    return maxValue;
}

// ---------------------------------------------------------------
// Main model update.
//
// Call exactly once per new IR sample (~every 50 ms).
//
// Input:
//     irRaw = ADC reading from optical IR sensor.
//
// Return:
//     final Model Pred in ADC-equivalent MP2 scale [0, 4095].
// ---------------------------------------------------------------
static inline float mp2ModelUpdate(
    MP2ModelState &state,
    float irRaw
) {
    // -----------------------------------------------------------
    // 1. IR baseline correction
    // Python:
    // IR_corrected = max(IR_raw - IR_baseline, 0)
    // -----------------------------------------------------------
    float irCorrected =
        irRaw - state.irBaseline;

    if (irCorrected < 0.0f) {
        irCorrected = 0.0f;
    }

    // -----------------------------------------------------------
    // 2. Causal EMA filter of corrected IR, tau = 0.25 s
    //
    // pandas ewm(adjust=False) initializes the first sample
    // directly from the first input value.
    // -----------------------------------------------------------
    if (!state.initialized) {
        state.irFiltered = irCorrected;
        state.irEma1s = state.irFiltered;
        state.irEma5s = state.irFiltered;
        state.irEma15s = state.irFiltered;

        state.initialized = true;
    }
    else {
        state.irFiltered +=
            MP2_ALPHA_IR_FILTER *
            (irCorrected - state.irFiltered);

        state.irEma1s +=
            MP2_ALPHA_EMA_1S *
            (state.irFiltered - state.irEma1s);

        state.irEma5s +=
            MP2_ALPHA_EMA_5S *
            (state.irFiltered - state.irEma5s);

        state.irEma15s +=
            MP2_ALPHA_EMA_15S *
            (state.irFiltered - state.irEma15s);
    }

    // -----------------------------------------------------------
    // 3. History-based features
    // -----------------------------------------------------------
    mp2PushHistory(
        state,
        state.irFiltered
    );

    const float irArea5s =
        mp2RecentSum(
            state,
            MP2_AREA_5S_SAMPLES
        ) * MP2_MODEL_DT_S;

    const float irArea15s =
        mp2RecentSum(
            state,
            MP2_AREA_15S_SAMPLES
        ) * MP2_MODEL_DT_S;

    const float irMax5s =
        mp2RecentMax(
            state,
            MP2_MAX_5S_SAMPLES
        );

    // -----------------------------------------------------------
    // 4. Quantile Regression equivalent linear equation
    //
    // mp2Raw = intercept + sum(w[i] * feature[i])
    // -----------------------------------------------------------
    const float features[MP2_NUM_FEATURES] = {
        state.irFiltered,
        state.irEma1s,
        state.irEma5s,
        state.irEma15s,
        irArea5s,
        irArea15s,
        irMax5s
    };

    float mp2Raw = MP2_INTERCEPT;

    for (int i = 0; i < MP2_NUM_FEATURES; ++i) {
        mp2Raw +=
            MP2_COEFFICIENTS[i] *
            features[i];
    }

    mp2Raw = mp2Clamp(
        mp2Raw,
        0.0f,
        4095.0f
    );

    // -----------------------------------------------------------
    // 5. Baseline gate
    //
    // activity = max(
    //   IR_filtered,
    //   IR_ema_1s,
    //   IR_ema_5s,
    //   IR_ema_15s,
    //   IR_max_5s
    // )
    //
    // gate = clip(activity / threshold, 0, 1)
    //
    // gated = baseline + gate * (raw - baseline)
    // -----------------------------------------------------------
    float activity = state.irFiltered;

    if (state.irEma1s > activity) {
        activity = state.irEma1s;
    }

    if (state.irEma5s > activity) {
        activity = state.irEma5s;
    }

    if (state.irEma15s > activity) {
        activity = state.irEma15s;
    }

    if (irMax5s > activity) {
        activity = irMax5s;
    }

    const float gate = mp2Clamp(
        activity / MP2_GATE_THRESHOLD,
        0.0f,
        1.0f
    );

    float gatedPrediction =
        MP2_BASELINE +
        gate * (
            mp2Raw - MP2_BASELINE
        );

    gatedPrediction = mp2Clamp(
        gatedPrediction,
        0.0f,
        4095.0f
    );

    // -----------------------------------------------------------
    // 6. Output EMA, tau = 0.5 s
    //
    // modelPred += alpha * (gated - modelPred)
    // -----------------------------------------------------------
    state.modelPred +=
        MP2_ALPHA_OUTPUT *
        (
            gatedPrediction -
            state.modelPred
        );

    state.modelPred = mp2Clamp(
        state.modelPred,
        0.0f,
        4095.0f
    );

    return state.modelPred;
}
