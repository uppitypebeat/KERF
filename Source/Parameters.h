#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Host-automatable parameters.

    IDs are permanent: renaming one breaks saved projects and automation, so only add new
    ones. Per-chop settings are not here; they live in the chop model (ChopModel). */
namespace kerf::Params
{
    // Output
    inline constexpr auto master        = "master";
    // Pitch & time
    inline constexpr auto globalPitch   = "pitch";
    inline constexpr auto stretchMode   = "stretch_mode";
    inline constexpr auto tempoSync     = "tempo_sync";
    // SPACE (stereo ping-pong delay)
    inline constexpr auto space         = "space";         // DELAY (the ID from when it was called SPACE)
    inline constexpr auto spaceTime     = "space_time";    // DELAY: its note value (PingPongDelay::Time)
    inline constexpr auto spaceMode     = "space_mode";    // DELAY: ping-pong or stereo
    // GRAIN
    inline constexpr auto grainMix      = "grain_mix";
    inline constexpr auto grainSize     = "grain_size";
    inline constexpr auto grainDensity  = "grain_density";
    inline constexpr auto grainSpray    = "grain_spray";
    inline constexpr auto grainPitchRnd = "grain_pitch_rand";
    inline constexpr auto grainSpread   = "grain_spread";
    inline constexpr auto grainScan     = "grain_scan";
    inline constexpr auto grainReverse  = "grain_reverse";
    inline constexpr auto grainPosition = "grain_position";
    inline constexpr auto grainPitch    = "grain_pitch";       // transpose of every grain (st)
    inline constexpr auto grainDrift    = "grain_drift";       // the read position wanders by itself
    inline constexpr auto grainShimmer  = "grain_shimmer";     // grains jumping octaves up
    inline constexpr auto grainShape    = "grain_shape";       // grain envelope: hard, soft, pluck
    inline constexpr auto grainScale    = "grain_scale";       // random pitch: free, octaves, fifths, chord
    inline constexpr auto grainFreeze   = "grain_freeze";      // hold the read head
    inline constexpr auto grainSync     = "grain_sync";        // DENSITY as note values at the tempo
    inline constexpr auto grainGlide    = "grain_glide";       // every grain bends up (+) or down (-) while it plays
    inline constexpr auto grainDetune   = "grain_detune";      // a fine random detune per grain (unison)
    inline constexpr auto grainHaze     = "grain_haze";        // a long, soft reverb on the grains only
    inline constexpr auto timeSlot      = "time_slot";         // TIME: off or one of 60 slots
    inline constexpr auto timeMix       = "time_mix";
    inline constexpr auto timeLength    = "time_length";       // the cycle: 1/2, 1 or 2 bars
    // A double-click on a page tab switches that page off (its settings stay).
    inline constexpr auto grainOff      = "grain_off";
    inline constexpr auto fxOff         = "fx_off";
    inline constexpr auto eqOff         = "eq_off";
    inline constexpr auto timeOff       = "time_off";
    // TONE
    inline constexpr auto filter        = "filter";
    inline constexpr auto resonance     = "resonance";
    // DIST (distortion, once DIRT). "drive" keeps its old ID so saved projects keep their value.
    inline constexpr auto drive         = "drive";
    inline constexpr auto dirtMode      = "dirt_mode";
    // WOBBLE (tape wow / chorus)
    inline constexpr auto wobble        = "wobble";
    inline constexpr auto wobbleMode    = "wobble_mode";
    // PUMP (tempo-locked ducking; took the place of VERB)
    inline constexpr auto pump          = "pump";
    inline constexpr auto pumpMode      = "pump_mode";
    // EQ: up to 8 points. A point exists when its "on" parameter is set (eq1_on .. eq8_q).
    inline constexpr int eqBands = 8;
    namespace EqField
    {
        inline constexpr auto on = "on", type = "type", freq = "freq", gain = "gain", q = "q";
        inline constexpr auto bypass = "bypass"; // the point stays, but is not heard (right-click on it)
    }
    /** e.g. eqParamID (0, EqField::freq) == "eq1_freq". */
    inline juce::String eqParamID (int band, const char* field)
    {
        return "eq" + juce::String (band + 1) + "_" + field;
    }
    /** The fixed three-band EQ before v0.4.5 (no longer parameters; old projects are migrated). */
    namespace LegacyEq
    {
        inline constexpr auto lowGain = "eq_low_gain", lowFreq = "eq_low_freq", midGain = "eq_mid_gain", midFreq = "eq_mid_freq",
                              midQ = "eq_mid_q", highGain = "eq_high_gain", highFreq = "eq_high_freq";
    }
    // Pattern
    inline constexpr auto patternPlay   = "pattern_play";
    inline constexpr auto swing         = "pattern_swing";    // 0 = straight .. 1 = 75 % swing
    inline constexpr auto humanize      = "pattern_humanize"; // timing / velocity variation
    // Voices
    inline constexpr auto voices        = "voices";
    inline constexpr auto retrigger     = "retrigger";
    inline constexpr auto quantize      = "trigger_quantize";

    // Pads: automatable per-chop settings for pads 1..32 (pad k = MIDI note root + k).
    namespace PadField
    {
        inline constexpr auto tune = "tune", fine = "fine", gain = "gain", pan = "pan", reverse = "reverse", mode = "mode",
                              fadeIn = "fade_in", fadeOut = "fade_out", spaceSend = "space_send", grainSend = "grain_send",
                              decay = "decay", sustain = "sustain";
    }

    /** e.g. padParamID (0, PadField::tune) == "pad01_tune". */
    inline juce::String padParamID (int padIndex, const char* field)
    {
        return "pad" + juce::String (padIndex + 1).paddedLeft ('0', 2) + "_" + field;
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
