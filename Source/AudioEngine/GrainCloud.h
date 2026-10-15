#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "ChopTypes.h"

namespace kerf
{

/** Global GRAIN settings for one block. */
struct GrainSettings
{
    float mix = 0.0f;          // TEXTURE: 0 = dry playback only, 1 = grains only
    float sizeMs = 80.0f;      // grain length
    float density = 20.0f;     // grains per second
    float spray = 0.1f;        // position randomness, fraction of the chop length
    float pitchRandom = 0.0f;  // +/- semitones per grain
    float spread = 0.5f;       // stereo randomness 0..1
    float shape = 0.25f;       // SHAPE: 0 = hard (flat top, gritty), 0.5 = soft bell, 1 = pluck (sharp attack, decay)
    float scan = 1.0f;         // read-head speed relative to normal playback (0 = frozen)
    float position = 0.0f;     // 0..1 of the chop: where grains read, ahead of the scanning read head
    float reverse = 0.0f;      // probability that a grain plays backwards
    float pitch = 0.0f;        // transpose of every grain (semitones)
    int pitchQuantize = 0;     // random pitch: 0 = free, 1 = octaves, 2 = fifths, 3 = chord tones
    bool chordMinor = false;   // chord tones of a minor (else major) chord, from the sample's key
    float jitter = 0.25f;      // timing randomness 0..1
    bool freeze = false;       // hold the read head where it is
    float drift = 0.0f;        // 0..1: the read position wanders by itself (slow random walk)
    float shimmer = 0.0f;      // 0..1: chance that a grain jumps one or two octaves up
    float syncBeats = 0.0f;    // SYNC: beats between grains (0 = free, DENSITY in grains per second)
    double bpm = 120.0;        // the tempo SYNC follows

    bool active() const noexcept { return mix > 0.0005f; }
};

/** SYNC: the note values DENSITY steps through when grains follow the tempo. */
namespace GrainSync
{
    inline constexpr int numDivisions = 10;
    inline constexpr float beats[numDivisions] = { 4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f, 0.0625f };
    inline constexpr const char* names[numDivisions] = { "1 BAR", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64" };
    /** The division for DENSITY's knob position (0..1): further right, shorter notes. */
    inline int indexFor (float normalised) noexcept { return std::clamp ((int) std::lround (normalised * (numDivisions - 1)), 0, numDivisions - 1); }
}

/** A cloud of short grains read from one chop. Owned by a voice.
    Real-time safe: fixed grain pool, no allocation, deterministic RNG, all reads
    clamped to the sample. */
class GrainCloud
{
public:
    static constexpr int maxGrains = 64;

    void prepare (double hostSampleRate);
    /** A new note: grains read from `startPosition` on (where the voice starts), the same every time there. */
    void start (const ChopParams& chop, double startPosition);

    /** Renders the cloud into outL/outR (overwrites, does not add).
        `basePlaybackRate` is the voice's source-frames-per-output-sample rate (includes pitch).
        `playheadFrame` is the dry voice's current position (followed when scan == 1). */
    void render (const SampleData& sample, const ChopParams& chop, const GrainSettings& s,
                 double basePlaybackRate, float* outL, float* outR, int numSamples);

    /** For the UI: calls fn (position, level 0..1, semitones) for every sounding grain. */
    template <typename Fn>
    void forEachGrain (Fn&& fn) const
    {
        for (int i = 0; i < numActive; ++i)
        {
            const auto& g = grains[(size_t) i];
            fn (g.position, envelope (g.age, g.length, g.shape), g.semitones);
        }
    }

    /** Centre and half-width (source frames) of the area grains are drawn from, as of the last block. */
    double getSprayCentre() const noexcept    { return sprayCentre; }
    double getSprayHalfWidth() const noexcept { return sprayHalfWidth; }

private:
    struct Grain
    {
        bool active = false;
        double position = 0.0;  // source frames
        double step = 1.0;      // source frames per output sample (negative = reverse)
        int age = 0, length = 1;
        float gainL = 1.0f, gainR = 1.0f;
        float shape = 0.25f;    // envelope shape (see GrainSettings::shape)
        float envPhase = 0.0f, envStep = 0.0f; // position in the envelope table
        float semitones = 0.0f; // pitch offset (for the display)
    };

    float nextRandom() noexcept; // 0..1
    float bipolarRandom() noexcept { return nextRandom() * 2.0f - 1.0f; }
    void spawn (const SampleData& sample, const ChopParams& chop, const GrainSettings& s, double baseRate, int startAge, double head);
    static uint32_t hash32 (uint64_t k) noexcept;
    double triggerAt (int64_t k, double jitter) const noexcept; // grain k's place in the sample
    static float envelope (int age, int length, float shape) noexcept;

    std::array<Grain, maxGrains> grains {};
    int numActive = 0;              // active grains: the first numActive of the pool
    static constexpr int envTableSize = 512;
    std::array<float, envTableSize + 2> envTable {};
    float envTableShape = -1.0f;    // the SHAPE the table was made for
    double hostRate = 44100.0;
    double readHead = 0.0;          // scanning read head (source frames)
    double sprayCentre = 0.0, sprayHalfWidth = 0.0;
    double driftOffset = 0.0, driftTarget = 0.0; // -1..1, scaled by DRIFT to half the chop
    double samplesToDriftTarget = 0.0;
    double samplesToNextGrain = 0.0;
    double grid = 0.0;                   // source frames between grains (DENSITY / SYNC at the read head's speed)
    int64_t nextGrain = 0;               // the next grain on the grid
    int64_t driftSegment = 0;            // the stretch of the sample DRIFT's goal belongs to
    bool primed = false;                 // the first block of a note has set things up
    uint32_t rng = 0x12345678u;
};

} // namespace kerf
