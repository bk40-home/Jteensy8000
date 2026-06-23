// =============================================================================
// FilterShape.cpp  –  Per-filter shape table (data definitions)
// =============================================================================
//
// One row per VAFilterType, in enum order. Each comment explains WHY the
// numbers are what they are — perceived peak knee for that topology, audible
// HP/LP cutoff limits, and any topology-specific quirks.
//
// Source: filter_response_analysis.py + Zavalishin section references.
// =============================================================================

#include "FilterShape.h"

const FilterShape kFilterShape[FILTER_COUNT] =
{
    // ── SVF (2-pole, Zavalishin §4.1) ────────────────────────────────────────
    // R = 1/(2Q) already curves the perceived peak — γ = 0.7 is a mild lift.
    // LP/BP/AP cover the full musical range; HP and Notch lose audible bite
    // above ~6–8 kHz.
    /* FILTER_SVF_LP    */ {   40.0f, 14000.0f, 0.70f },
    /* FILTER_SVF_HP    */ {   30.0f,  6000.0f, 0.70f },
    /* FILTER_SVF_BP    */ {   40.0f, 14000.0f, 0.70f },
    /* FILTER_SVF_NOTCH */ {  100.0f,  8000.0f, 0.70f },
    /* FILTER_SVF_AP    */ {   30.0f, 20000.0f, 0.70f },

    // ── Moog ladder (Zavalishin §5.1) ────────────────────────────────────────
    // k⁴-like feedback gives the latest knee of any family. Strongest γ lift.
    // BP2 is the pole-subtraction tap — narrow useful range, tighter limits.
    /* FILTER_MOOG_LP4  */ {   40.0f, 12000.0f, 0.30f },
    /* FILTER_MOOG_LP2  */ {   40.0f, 12000.0f, 0.30f },
    /* FILTER_MOOG_BP2  */ {   80.0f, 10000.0f, 0.30f },

    // ── Diode ladder (Pirkle AN-6) ───────────────────────────────────────────
    // k spans 0..17 so the linear map already spreads the audible range better
    // than Moog — moderate γ lift only.
    /* FILTER_DIODE_LP  */ {   40.0f, 12000.0f, 0.45f },

    // ── Korg 35 / TSK (Zavalishin §5.8) ──────────────────────────────────────
    // tanh in the feedback path bounds Q growth, so audible resonance saturates
    // well before the nominal k=2 self-osc point — needs a stronger γ than the
    // raw threshold would suggest.
    /* FILTER_KORG35_LP */ {   40.0f, 12000.0f, 0.40f },
    /* FILTER_KORG35_HP */ {   30.0f,  6000.0f, 0.40f },

    // ── TPT 1-pole (Zavalishin §3.1) ─────────────────────────────────────────
    // No resonance — γ is unused (kept at 1.0 for table uniformity). Range
    // matches the gentle slope: LP is musical right up to 18 kHz, HP useful
    // only up to ~8 kHz before it becomes a click.
    /* FILTER_TPT1_LP   */ {   50.0f, 18000.0f, 1.00f },
    /* FILTER_TPT1_HP   */ {   30.0f,  8000.0f, 1.00f },
};
