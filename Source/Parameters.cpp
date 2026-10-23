#include "Parameters.h"
#include "Effects/TimeShaper.h"
#include "AudioEngine/GrainCloud.h"

namespace kerf::Params
{

namespace
{
    using FloatAttr = juce::AudioParameterFloatAttributes;

    juce::ParameterID pid (const char* id) { return { id, 1 }; }

    auto percent()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; })
                          .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("-0123456789.").getFloatValue() / 100.0f; });
    }

    auto decibels()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; })
                          .withLabel ("dB");
    }

    auto semitones()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int)
                          {
                              const auto r = juce::roundToInt (v);
                              return (r > 0 ? "+" : "") + juce::String (r) + " st";
                          })
                          .withLabel ("st");
    }

    auto milliseconds()
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " ms"; })
                          .withLabel ("ms");
    }

    juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    using F = juce::AudioParameterFloat;
    using C = juce::AudioParameterChoice;
    using B = juce::AudioParameterBool;
    using I = juce::AudioParameterInt;

    // Output -----------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (master), "Master", juce::NormalisableRange<float> (-60.0f, 6.0f, 0.0f, 2.5f), 0.0f, decibels()));

    // Pitch & time ---------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (globalPitch), "Global Pitch", juce::NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f, semitones()));
    layout.add (std::make_unique<C> (pid (stretchMode), "Stretch Mode", juce::StringArray { "Repitch", "Stretch" }, 0));
    layout.add (std::make_unique<B> (pid (tempoSync), "Tempo Sync", false));

    // SPACE ------------------------------------------------------------------------
    // One macro knob: tempo-locked ping-pong echoes; amount sets level, length, width and tone.
    layout.add (std::make_unique<F> (pid (space), "Echo", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));

    // GRAIN ------------------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (grainMix), "Grains", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainSize), "Grain Size", skewedRange (5.0f, 500.0f, 80.0f), 80.0f, milliseconds()));
    layout.add (std::make_unique<F> (pid (grainDensity), "Grain Density", skewedRange (1.0f, 120.0f, 20.0f), 20.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + " /s"; })));
    layout.add (std::make_unique<F> (pid (grainSpray), "Grain Spray", juce::NormalisableRange<float> (0.0f, 1.0f), 0.1f, percent()));
    layout.add (std::make_unique<F> (pid (grainPitchRnd), "Grain Pitch Random", juce::NormalisableRange<float> (0.0f, 12.0f, 1.0f), 0.0f, semitones()));
    layout.add (std::make_unique<F> (pid (grainSpread), "Grain Spread", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent()));
    layout.add (std::make_unique<F> (pid (grainScan), "Grain Speed", juce::NormalisableRange<float> (0.0f, 2.0f), 1.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2) + "x"; })));
    layout.add (std::make_unique<F> (pid (grainReverse), "Grain Reverse", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainPosition), "Grain Position", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainPitch), "Grain Pitch", juce::NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f, semitones()));
    layout.add (std::make_unique<F> (pid (grainDrift), "Grain Drift", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainShimmer), "Grain Shimmer", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainGlide), "Grain Glide", juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int)
                                     {
                                         const auto st = v * GrainLimits::glideSemitones;
                                         return std::abs (st) < 0.05f ? juce::String ("0 st") : (st > 0.0f ? "+" : "") + juce::String (st, 1) + " st";
                                     })));
    layout.add (std::make_unique<F> (pid (grainDetune), "Grain Detune", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int)
                                     { return juce::String (juce::roundToInt (v * GrainLimits::detuneCents)) + " ct"; })));
    layout.add (std::make_unique<F> (pid (grainHaze), "Grain Haze", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<F> (pid (grainShape), "Grain Shape", juce::NormalisableRange<float> (0.0f, 1.0f), 0.25f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int)
                                     {
                                         if (v < 0.08f) return juce::String ("HARD");
                                         if (std::abs (v - 0.5f) < 0.04f) return juce::String ("SOFT");
                                         if (v > 0.92f) return juce::String ("PLUCK");
                                         return juce::String (juce::roundToInt (v * 100.0f)) + "%";
                                     })));
    layout.add (std::make_unique<C> (pid (grainScale), "Grain Pitch Scale", juce::StringArray { "Free", "Octaves", "Fifths", "Chord" }, 0));
    layout.add (std::make_unique<B> (pid (grainFreeze), "Grain Freeze", false));
    layout.add (std::make_unique<B> (pid (grainSync), "Grain Sync", false));

    // TIME --------------------------------------------------------------------------
    {
        juce::StringArray slots { "Off" };
        for (int k = 0; k < TimeFactory::numSlots; ++k)
            slots.add (juce::String (k + 1) + " " + TimeFactory::name (k));
        layout.add (std::make_unique<C> (pid (timeSlot), "Time Slot", slots, 0));
    }
    layout.add (std::make_unique<F> (pid (timeMix), "Time Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent()));
    layout.add (std::make_unique<C> (pid (timeLength), "Time Length", juce::StringArray { "1/2 Bar", "1 Bar", "2 Bars" }, 1));

    // Page switches (a double-click on the tab) ------------------------------------
    layout.add (std::make_unique<B> (pid (grainOff), "Granulizer Off", false));
    layout.add (std::make_unique<B> (pid (fxOff), "FX Off", false));
    layout.add (std::make_unique<B> (pid (eqOff), "EQ Off", false));
    layout.add (std::make_unique<B> (pid (timeOff), "Time Off", false));

    // TONE -------------------------------------------------------------------------
    // One-knob filter: left of centre = low-pass, right of centre = high-pass, centre = off.
    layout.add (std::make_unique<F> (pid (filter), "Filter", juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int)
                                     {
                                         if (std::abs (v) < 0.01f) return juce::String ("OFF");
                                         return juce::String (v < 0 ? "LP " : "HP ") + juce::String (juce::roundToInt (std::abs (v) * 100.0f));
                                     })));
    layout.add (std::make_unique<F> (pid (resonance), "Resonance", juce::NormalisableRange<float> (0.0f, 1.0f), 0.2f, percent()));
    // DIRT -------------------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (drive), "Dirt", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<C> (pid (dirtMode), "Dirt Mode", juce::StringArray { "Warm", "Fuzz", "Crush" }, 0));

    // WOBBLE -----------------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (wobble), "Wobble", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<C> (pid (wobbleMode), "Wobble Mode", juce::StringArray { "Tape", "Chorus" }, 0));

    // EQ ---------------------------------------------------------------------------
    auto hz = [] { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz"; }); };
    auto eqDb = [] { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }); };
    for (int band = 0; band < eqBands; ++band)
    {
        const auto label = "EQ " + juce::String (band + 1);
        auto id = [band] (const char* field) { return juce::ParameterID { eqParamID (band, field), 1 }; };
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("eq" + juce::String (band + 1), label, " | ");
        group->addChild (std::make_unique<B> (id (EqField::on), label + " On", false));
        group->addChild (std::make_unique<B> (id (EqField::bypass), label + " Bypass", false));
        group->addChild (std::make_unique<C> (id (EqField::type), label + " Type",
                                              juce::StringArray { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" }, 0));
        group->addChild (std::make_unique<F> (id (EqField::freq), label + " Freq", skewedRange (20.0f, 30000.0f, 630.0f), 1000.0f, hz()));
        group->addChild (std::make_unique<F> (id (EqField::gain), label + " Gain", juce::NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, eqDb()));
        group->addChild (std::make_unique<F> (id (EqField::q), label + " Q", skewedRange (0.2f, 10.0f, 1.0f), 0.9f,
                                              FloatAttr().withStringFromValueFunction ([] (float v, int) { return "Q " + juce::String (v, 2); })));
        layout.add (std::move (group));
    }

    // PUMP ------------------------------------------------------------------------
    layout.add (std::make_unique<F> (pid (pump), "Pump", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));
    layout.add (std::make_unique<C> (pid (pumpMode), "Pump Rate", juce::StringArray { "1/4", "1/8", "1/16" }, 0));

    // Pattern ------------------------------------------------------------------------
    layout.add (std::make_unique<B> (pid (patternPlay), "Pattern Play", false));
    layout.add (std::make_unique<F> (pid (swing), "Swing", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
                                     FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (50.0f + 25.0f * v)) + "%"; })));
    layout.add (std::make_unique<F> (pid (humanize), "Humanize", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent()));

    // Voices -----------------------------------------------------------------------
    layout.add (std::make_unique<I> (pid (voices), "Voices", 1, 32, 16));
    layout.add (std::make_unique<C> (pid (retrigger), "Retrigger", juce::StringArray { "Poly", "Same note cuts", "Mono" }, 1));
    layout.add (std::make_unique<C> (pid (quantize), "Trigger Quantize", juce::StringArray { "Off", "1/16", "1/8", "1/4", "1 Bar" }, 0));

    // Pads 01..32: automatable sound settings of the chops on root .. root + 31 ------------
    auto signedText = [] (const juce::String& unit)
    {
        return FloatAttr().withStringFromValueFunction ([unit] (float v, int)
        {
            const auto r = juce::roundToInt (v);
            return (r > 0 ? "+" : "") + juce::String (r) + " " + unit;
        });
    };
    auto msText = [] { return FloatAttr().withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + " ms"; }); };
    auto panText = []
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int)
        {
            const auto r = juce::roundToInt (v * 100.0f);
            return r == 0 ? juce::String ("C") : (r < 0 ? "L" + juce::String (-r) : "R" + juce::String (r));
        });
    };
    auto gainText = []
    {
        return FloatAttr().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? juce::String ("-inf") : juce::String (v, 1) + " dB"; });
    };

    for (int pad = 0; pad < 32; ++pad)
    {
        const auto label = "Pad " + juce::String (pad + 1).paddedLeft ('0', 2);
        auto id = [pad] (const char* field) { return juce::ParameterID { padParamID (pad, field), 1 }; };

        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("pad" + juce::String (pad + 1), label, " | ");
        group->addChild (std::make_unique<F> (id (PadField::tune), label + " Tune", juce::NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f, signedText ("st")));
        group->addChild (std::make_unique<F> (id (PadField::fine), label + " Fine", juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f, signedText ("ct")));
        juce::NormalisableRange<float> gainRange (-60.0f, 12.0f, 0.1f);
        gainRange.setSkewForCentre (-12.0f);
        group->addChild (std::make_unique<F> (id (PadField::gain), label + " Gain", gainRange, 0.0f, gainText()));
        group->addChild (std::make_unique<F> (id (PadField::pan), label + " Pan", juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f, panText()));
        group->addChild (std::make_unique<B> (id (PadField::reverse), label + " Reverse", false));
        group->addChild (std::make_unique<C> (id (PadField::mode), label + " Mode", juce::StringArray { "One-shot", "Gate", "Loop" }, 1));
        group->addChild (std::make_unique<F> (id (PadField::fadeIn), label + " Fade In", skewedRange (0.0f, 1000.0f, 60.0f), 1.0f, msText()));
        group->addChild (std::make_unique<F> (id (PadField::fadeOut), label + " Fade Out", skewedRange (0.0f, 2000.0f, 120.0f), 8.0f, msText()));
        group->addChild (std::make_unique<F> (id (PadField::spaceSend), label + " Echo Send", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent()));
        group->addChild (std::make_unique<F> (id (PadField::grainSend), label + " Grain Amount", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent()));
        group->addChild (std::make_unique<F> (id (PadField::decay), label + " Decay", skewedRange (1.0f, 5000.0f, 300.0f), 300.0f, msText()));
        group->addChild (std::make_unique<F> (id (PadField::sustain), label + " Sustain", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent()));
        layout.add (std::move (group));
    }

    return layout;
}

} // namespace kerf::Params
