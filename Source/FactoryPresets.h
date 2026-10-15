#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <vector>

namespace kerf::Factory
{

/** A sound that ships with KERF. Factory presets shape the sound around your chops -
    effects, granulizer, EQ, groove and master level - and leave the chops, the pads, the
    key and the voice settings alone. Everything in that scope they do not mention goes
    back to its default, so each preset sounds the same whatever was loaded before. */
struct Preset
{
    struct EqPoint { int type; float freq, gainDb, q; }; // type: EqProcessor::Type
    const char* name;
    const char* category;
    const char* description;
    std::vector<std::pair<const char*, float>> values; // parameter id, real value
    std::vector<EqPoint> eq;
};

const std::vector<Preset>& presets();
juce::StringArray categories(); // in menu order

/** True for the parameters a factory preset controls. */
bool inScope (const juce::String& parameterId);

/** The preset as a SOUND tree (id / value for every parameter in scope that it sets;
    the rest of the scope is left out, i.e. defaults). */
juce::ValueTree toSound (const Preset& preset);

} // namespace kerf::Factory
