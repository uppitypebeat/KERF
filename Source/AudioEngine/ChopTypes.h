#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include "SampleData.h"
#include "../Arrangement/PatternData.h"
#include "../Effects/TimeShaper.h"

namespace kerf
{

constexpr int maxChops = 256;   // the first 128 can sit on MIDI notes; all of them play from the pads
constexpr int defaultRootNote = 60; // MIDI 60 = middle C
/** Octave number of middle C in note names. FL Studio calls MIDI 60 "C5" (others say C4
    or C3), so KERF names notes the way the FL piano roll shows them. */
constexpr int middleCOctave = 5;

/** What happens when a chop's MIDI note is pressed and released. */
enum class PlayMode : int
{
    oneShot = 0, // plays to the end of the chop, ignores note-off
    gate    = 1, // plays while the key is held, fades out on release
    loop    = 2  // loops the chop while held, fades out on release
};

/** All per-chop settings. Plain data so it can be copied into an audio-thread snapshot. */
struct ChopParams
{
    int id = 0;                 // stable identity, never reused within a session
    int64_t start = 0;          // in source-sample frames
    int64_t end = 0;            // exclusive
    int note = -1;              // MIDI note, -1 = unassigned
    PlayMode mode = PlayMode::gate;
    float pitchSemitones = 0.0f;   // -24..+24
    float fineCents = 0.0f;        // -100..+100
    bool reverse = false;
    float gainDb = 0.0f;           // -60..+12
    float pan = 0.0f;              // -1..+1
    float fadeInMs = 1.0f;         // 0..1000  (a minimum ramp prevents clicks)
    float fadeOutMs = 8.0f;        // 0..2000
    float decayMs = 300.0f;        // 1..5000: after the attack, the level falls to sustain in this time
    float sustain = 1.0f;          // 0..1: the level it holds (1 = no decay, the classic playback)
    float spaceSend = 1.0f;        // 0..1, scales the global SPACE amount for this chop
    float grainSend = 1.0f;        // 0..1, scales the global GRAIN texture for this chop
    int chokeGroup = 0;            // 0 = none; chops sharing a group cut each other off

    int64_t length() const noexcept { return end - start; }
};

namespace ChopLimits
{
    constexpr float minPitch = -24.0f, maxPitch = 24.0f;
    constexpr float minFine = -100.0f, maxFine = 100.0f;
    constexpr float minGainDb = -60.0f, maxGainDb = 12.0f;
    constexpr float maxFadeInMs = 1000.0f, maxFadeOutMs = 2000.0f;
    constexpr float minDecayMs = 1.0f, maxDecayMs = 5000.0f;
    constexpr int64_t minLength = 32; // frames — shorter chops are not allowed
    constexpr int maxChokeGroup = 4;
}

/** Combined pitch of a chop, in semitones, including the global transpose.
    Global and per-chop pitch simply add: a chop at +3 st with global -12 st plays at -9 st. */
inline double totalSemitones (const ChopParams& c, double globalSemitones) noexcept
{
    return globalSemitones + (double) c.pitchSemitones + (double) c.fineCents / 100.0;
}

inline double semitonesToRatio (double semitones) noexcept
{
    return std::pow (2.0, semitones / 12.0);
}

/** Pads: the sound settings of the chops on the first 32 notes from the root note are
    host parameters ("Pad 01 Tune" ...), so they can be automated. Pad k plays the chop on
    MIDI note root + k. Chops outside that range keep their settings in the chop model. */
constexpr int numPads = 32;

struct PadSound
{
    float pitchSemitones = 0.0f, fineCents = 0.0f, gainDb = 0.0f, pan = 0.0f;
    float fadeInMs = 1.0f, fadeOutMs = 8.0f, spaceSend = 1.0f, grainSend = 1.0f;
    float decayMs = 300.0f, sustain = 1.0f;
    bool reverse = false;
    PlayMode mode = PlayMode::gate;

    void applyTo (ChopParams& c) const noexcept
    {
        c.pitchSemitones = pitchSemitones;
        c.fineCents = fineCents;
        c.gainDb = gainDb;
        c.pan = pan;
        c.fadeInMs = fadeInMs;
        c.fadeOutMs = fadeOutMs;
        c.decayMs = decayMs;
        c.sustain = sustain;
        c.spaceSend = spaceSend;
        c.grainSend = grainSend;
        c.reverse = reverse;
        c.mode = mode;
    }

    static PadSound from (const ChopParams& c) noexcept
    {
        PadSound p;
        p.pitchSemitones = c.pitchSemitones; p.fineCents = c.fineCents; p.gainDb = c.gainDb; p.pan = c.pan;
        p.fadeInMs = c.fadeInMs; p.fadeOutMs = c.fadeOutMs; p.spaceSend = c.spaceSend; p.grainSend = c.grainSend;
        p.decayMs = c.decayMs; p.sustain = c.sustain;
        p.reverse = c.reverse; p.mode = c.mode;
        return p;
    }
};

/** Current pad parameter values, refreshed by the processor every block (audio thread). */
struct PadTable
{
    std::array<PadSound, numPads> pads {};
    int rootNote = defaultRootNote;

    const PadSound* forNote (int note) const noexcept
    {
        const auto slot = note - rootNote;
        return slot >= 0 && slot < numPads ? &pads[(size_t) slot] : nullptr;
    }
};

/** Immutable view of the instrument handed to the audio thread. Rebuilt on the message
    thread whenever the sample or any chop changes. */
struct EngineSnapshot final : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<EngineSnapshot>;

    SampleData::Ptr sample;
    std::array<ChopParams, maxChops> chops {};
    int numChops = 0;
    std::array<int16_t, 128> chopIndexForNote {}; // -1 = no chop on that note
    double sourceBpm = 0.0;                        // 0 = unknown
    double beatOrigin = 0.0;                       // SHIFT: the frame where the beat grid starts

    // Stretch mode: pre-rendered (pitch-independent, tempo-matched) audio per chop.
    // nullptr = not rendered (yet); voices then fall back to repitch playback.
    bool stretchMode = false;
    std::array<SampleData::Ptr, maxChops> rendered {};

    PatternData pattern;

    std::array<TimeSlot, TimeFactory::numSlots> timeSlots {}; // TIME: the curves of every slot

    EngineSnapshot() { chopIndexForNote.fill (-1); }

    int chopIndexFor (int note) const noexcept
    {
        return note >= 0 && note <= 127 ? chopIndexForNote[(size_t) note] : -1;
    }

    const ChopParams* chopForNote (int note) const noexcept
    {
        if (note < 0 || note > 127)
            return nullptr;

        const auto idx = chopIndexForNote[(size_t) note];
        return idx >= 0 && idx < numChops ? &chops[(size_t) idx] : nullptr;
    }

    const ChopParams* chopWithId (int id) const noexcept
    {
        for (int i = 0; i < numChops; ++i)
            if (chops[(size_t) i].id == id)
                return &chops[(size_t) i];
        return nullptr;
    }
};

} // namespace kerf
