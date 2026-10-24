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

/** RANDOM: a new sound from `seed` - two to five of the effects (DELAY, DISTORTION, FILTER,
    WOBBLE, PUMP, the granulizer, TIME, EQ), each with its own random settings in a range
    that makes music, the rest off; the level matched. `description` says what it rolled. */
juce::ValueTree randomSound (uint32_t seed, juce::String& description);

} // namespace kerf::Factory
