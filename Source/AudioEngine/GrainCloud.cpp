#include "GrainCloud.h"

namespace kerf
{

void GrainCloud::prepare (double hostSampleRate)
{
    hostRate = hostSampleRate;
    for (auto& g : grains)
        g.active = false;
    numActive = 0;
}

void GrainCloud::start (const ChopParams& chop, double startPosition)
{
    for (auto& g : grains)
        g.active = false;
    numActive = 0;

    // From where the voice starts (not the chop's start), and with nothing left to chance
    // from one hit to the next: the same place sounds the same every time.
    readHead = juce::jlimit ((double) chop.start, std::max ((double) chop.start, (double) chop.end - 1.0), startPosition);
    rng = hash32 ((uint64_t) (int64_t) std::floor (readHead) ^ 0x6b65726600000000ull);
    samplesToNextGrain = 0.0; // (frozen read head: the first grain at once)
    driftOffset = driftTarget = 0.0;
    samplesToDriftTarget = 0.0;
    driftSegment = std::numeric_limits<int64_t>::min();
    grid = 0.0; // the grain grid is set up on the first block
    primed = false;
}

uint32_t GrainCloud::hash32 (uint64_t k) noexcept
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return (uint32_t) k | 1u; // (xorshift needs a seed that is not 0)
}

double GrainCloud::triggerAt (int64_t k, double jitter) const noexcept
{
    // Grain k's place in the sample: on the grid, nudged by its own jitter (+/- half a step at most).
    const auto u = (double) (hash32 ((uint64_t) k * 0x9e3779b97f4a7c15ull) & 0xffffff) / (double) 0x1000000;
    return ((double) k + jitter * 0.5 * (u * 2.0 - 1.0)) * grid;
}

float GrainCloud::nextRandom() noexcept
{
    // xorshift32: fast, allocation-free, deterministic per voice.
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float) (rng & 0xffffff) / (float) 0x1000000;
}

float GrainCloud::envelope (int age, int length, float shape) noexcept
{
    // SHAPE 0 .. 0.5: a Tukey window from nearly rectangular (hard, gritty) to Hann (soft);
    // 0.5 .. 1: the bell morphs into a pluck (a 2 % attack, then a falling tail).
    const auto x = (float) age / (float) std::max (1, length - 1);
    const auto pi = juce::MathConstants<float>::pi;
    auto tukey = [x, pi] (float taper)
    {
        const auto edge = juce::jlimit (0.02f, 1.0f, taper) * 0.5f;
        if (x < edge)        return 0.5f * (1.0f - std::cos (pi * x / edge));
        if (x > 1.0f - edge) return 0.5f * (1.0f - std::cos (pi * (1.0f - x) / edge));
        return 1.0f;
    };

    shape = juce::jlimit (0.0f, 1.0f, shape);
    if (shape <= 0.5f)
        return tukey (0.1f + 0.9f * shape * 2.0f);

    constexpr float attack = 0.02f;
    const auto pluck = x < attack ? 0.5f * (1.0f - std::cos (pi * x / attack))
                                  : std::exp (-(x - attack) * 6.0f) * (x > 0.9f ? 0.5f * (1.0f + std::cos (pi * (x - 0.9f) / 0.1f)) : 1.0f);
    const auto t = (shape - 0.5f) * 2.0f;
    return (1.0f - t) * tukey (1.0f) + t * 1.4f * pluck; // a pluck carries less energy: a little lift
}

void GrainCloud::spawn (const SampleData& sample, const ChopParams& chop, const GrainSettings& s, double baseRate, int startAge, double head)
{
    // Active grains are kept together at the front of the pool: the next one goes after them.
    if (numActive >= maxGrains)
        return; // pool full: skip this grain rather than steal (keeps cost bounded)
    Grain* slot = &grains[(size_t) numActive++];

    const auto regionStart = (double) chop.start;
    const auto regionLen = std::max (1.0, (double) chop.length());

    // Where the grain starts reading: the read head, POSITION ahead of it, DRIFT's wander,
    // and SPRAY's scatter around that.
    double pos = head + juce::jlimit (0.0f, 1.0f, s.position) * regionLen
               + driftOffset * juce::jlimit (0.0f, 1.0f, s.drift) * 0.5 * regionLen;
    pos += bipolarRandom() * s.spray * 0.5 * regionLen;

    // Keep inside the chop (wrap around).
    pos = regionStart + std::fmod (std::fmod (pos - regionStart, regionLen) + regionLen, regionLen);

    // Grain pitch: transpose + a random offset, free or one of a set of intervals (octaves,
    // fifths, or the tones of a major / minor chord) within the RANDOM range, all equally
    // likely.
    auto offset = bipolarRandom() * s.pitchRandom;
    if (s.pitchQuantize > 0)
    {
        static constexpr float octaves[] = { -24.0f, -12.0f, 0.0f, 12.0f, 24.0f };
        static constexpr float fifths[] = { -24.0f, -17.0f, -12.0f, -5.0f, 0.0f, 7.0f, 12.0f, 19.0f, 24.0f };
        static constexpr float minorChord[] = { -24.0f, -21.0f, -17.0f, -12.0f, -9.0f, -5.0f, 0.0f, 3.0f, 7.0f, 12.0f, 15.0f, 19.0f, 24.0f };
        static constexpr float majorChord[] = { -24.0f, -20.0f, -17.0f, -12.0f, -8.0f, -5.0f, 0.0f, 4.0f, 7.0f, 12.0f, 16.0f, 19.0f, 24.0f };
        const float* set = octaves;
        int size = (int) std::size (octaves);
        if (s.pitchQuantize == 2)      { set = fifths; size = (int) std::size (fifths); }
        else if (s.pitchQuantize == 3) { set = s.chordMinor ? minorChord : majorChord; size = (int) std::size (minorChord); }

        float allowed[16];
        int count = 0;
        for (int i = 0; i < size; ++i)
            if (std::abs (set[i]) <= s.pitchRandom + 0.5f)
                allowed[count++] = set[i];
        offset = count > 0 ? allowed[std::min (count - 1, (int) (nextRandom() * (float) count))] : 0.0f;
    }
    // SHIMMER: some grains an octave up (a few two), a bright layer over the cloud.
    if (s.shimmer > 0.0005f && nextRandom() < 0.7f * s.shimmer)
        offset += nextRandom() < 0.3f ? 24.0f : 12.0f;

    const auto semis = s.pitch + offset;
    const auto pitch = semitonesToRatio ((double) semis);
    const bool backwards = nextRandom() < s.reverse;
    const auto direction = (chop.reverse != backwards) ? -1.0 : 1.0;

    slot->active = true;
    slot->step = baseRate * pitch * direction;
    slot->length = std::max (16, (int) (s.sizeMs * 0.001 * hostRate));
    slot->age = std::min (startAge, slot->length - 1);
    slot->position = pos + slot->step * slot->age; // (already that far, when it began earlier)
    slot->position = regionStart + std::fmod (std::fmod (slot->position - regionStart, regionLen) + regionLen, regionLen);
    slot->shape = s.shape;
    slot->envStep = (float) envTableSize / (float) std::max (1, slot->length - 1);
    slot->envPhase = std::min ((float) envTableSize, slot->envStep * (float) slot->age);
    slot->semitones = semis;

    // Constant-power random pan within the spread.
    const auto pan = bipolarRandom() * juce::jlimit (0.0f, 1.0f, s.spread);
    const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    slot->gainL = std::cos (angle) * juce::MathConstants<float>::sqrt2;
    slot->gainR = std::sin (angle) * juce::MathConstants<float>::sqrt2;

    juce::ignoreUnused (sample);
}

void GrainCloud::render (const SampleData& sample, const ChopParams& chop, const GrainSettings& s,
                         double baseRate, float* outL, float* outR, int numSamples)
{
    std::fill (outL, outL + numSamples, 0.0f);
    std::fill (outR, outR + numSamples, 0.0f);

    const auto total = sample.getNumSamples();
    if (total < 4)
        return;

    const auto* srcL = sample.audio.getReadPointer (0);
    const auto* srcR = sample.audio.getReadPointer (sample.getNumChannels() > 1 ? 1 : 0);

    // Free: DENSITY grains per second. SYNC: one grain per note value at the tempo, exactly on
    // time (no jitter), so the cloud becomes a rhythm.
    const auto synced = s.syncBeats > 0.0f && s.bpm > 0.0;
    const auto interval = synced ? (double) s.syncBeats * 60.0 / juce::jlimit (20.0, 400.0, s.bpm) * hostRate
                                 : hostRate / juce::jlimit (0.5, 400.0, (double) s.density);
    const auto regionStart = (double) chop.start;
    const auto regionLen = std::max (1.0, (double) chop.length());
    // The read head moves at SCAN times the playback speed (0 = it stays where the note began).
    const auto headStep = s.freeze ? 0.0 : baseRate * juce::jlimit (0.0, 4.0, (double) s.scan) * (chop.reverse ? -1.0 : 1.0);

    // DRIFT: the read position wanders - a new random goal now and then, glided to. While the
    // read head moves, the goals belong to places in the sample (every 0.6 s of it), so they
    // come back the same way; frozen, every 0.3 .. 0.9 s.
    const auto stepAbs = std::abs (headStep);
    const auto positional = stepAbs > 1.0e-6; // the grains follow the read head through the sample
    if (s.drift > 0.0005f)
    {
        if (positional)
        {
            const auto segment = (int64_t) std::floor (readHead / (0.6 * hostRate * stepAbs));
            if (segment != driftSegment)
            {
                driftSegment = segment;
                driftTarget = (double) (hash32 ((uint64_t) segment ^ 0x64726966ull) & 0xffffff) / (double) 0x1000000 * 2.0 - 1.0;
                if (! primed)
                    driftOffset = driftTarget;
            }
        }
        else if ((samplesToDriftTarget -= numSamples) <= 0.0)
        {
            driftTarget = bipolarRandom();
            samplesToDriftTarget = hostRate * (0.3 + 0.6 * nextRandom());
        }
        driftOffset += (driftTarget - driftOffset) * (1.0 - std::exp (-numSamples / (0.35 * hostRate)));
    }

    // For the UI: where grains are currently drawn from.
    const auto centre = readHead + juce::jlimit (0.0f, 1.0f, s.position) * regionLen
                      + driftOffset * juce::jlimit (0.0f, 1.0f, s.drift) * 0.5 * regionLen;
    sprayCentre = regionStart + std::fmod (std::fmod (centre - regionStart, regionLen) + regionLen, regionLen);
    sprayHalfWidth = (double) s.spray * 0.5 * regionLen;

    // Level normalisation: the number of grains expected to overlap (no more than the pool).
    const auto overlap = juce::jlimit (1.0, (double) maxGrains, hostRate / interval * s.sizeMs * 0.001);
    const auto norm = (float) (1.0 / std::sqrt (overlap));

    // 4-point Hermite interpolation: clean when grains are pitched (linear dulls and grits).
    auto read = [total] (const float* d, double p) noexcept
    {
        const auto i = (int64_t) std::floor (p);
        const auto f = (float) (p - (double) i);
        float xm1, x0, x1, x2;
        if (i >= 1 && i + 2 < total)
        {
            xm1 = d[i - 1]; x0 = d[i]; x1 = d[i + 1]; x2 = d[i + 2];
        }
        else
        {
            auto at = [d, total] (int64_t k) { return d[juce::jlimit<int64_t> (0, total - 1, k)]; };
            xm1 = at (i - 1); x0 = at (i); x1 = at (i + 1); x2 = at (i + 2);
        }
        const auto c1 = 0.5f * (x1 - xm1);
        const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    };

    // SHAPE as a table for this block (every grain reads it); rebuilt when SHAPE moves.
    if (s.shape != envTableShape)
    {
        envTableShape = s.shape;
        for (int k = 0; k <= envTableSize; ++k)
            envTable[(size_t) k] = envelope (k, envTableSize + 1, s.shape);
        envTable[(size_t) envTableSize + 1] = envTable[(size_t) envTableSize];
    }
    const auto mono = srcL == srcR;

    // When grains start. With the read head moving (SCAN), grain k starts where the head passes
    // its place in the sample: k steps of the grid (the DENSITY or SYNC interval, measured in
    // the sample), each nudged by its jitter, and everything about it (pitch, pan, scatter)
    // drawn from k alone. So a place in the sample sounds the same however playback got
    // there: started there, 5 ms earlier, or played through. The grains that would already
    // be sounding at the start are there too, part-way through. Frozen (FREEZE, SCAN 0), it
    // counts time instead.
    // Jitter: 0 = metronomic, 1 = very loose timing (avoids a mechanical buzz at high densities).
    const auto jitter = synced ? 0.0 : (double) juce::jlimit (0.0f, 1.0f, s.jitter) * 0.95;
    const auto forward = headStep > 0.0;
    const auto grainLength = std::max (16, (int) (s.sizeMs * 0.001 * hostRate));
    auto spawnGrain = [&] (int64_t k, int age)
    {
        rng = hash32 ((uint64_t) k);
        spawn (sample, chop, s, baseRate, age, triggerAt (k, jitter)); // from where the head was when it began
    };
    auto resync = [&]
    {
        nextGrain = forward ? (int64_t) std::floor (readHead / grid) + 1 : (int64_t) std::ceil (readHead / grid) - 1;
        // (A nudged neighbour can lie on the other side of the head: take it next.)
        if (forward ? triggerAt (nextGrain - 1, jitter) > readHead : triggerAt (nextGrain + 1, jitter) < readHead)
            nextGrain += forward ? -1 : 1;
    };
    if (positional)
    {
        const auto newGrid = interval * stepAbs;
        if (! primed)
        {
            grid = newGrid;
            resync();
            // The grains that began before this place and still sound.
            const auto reach = (double) grainLength * stepAbs;
            int64_t k = nextGrain - (forward ? 1 : -1);
            for (int count = 0; count < maxGrains; ++count, k += forward ? -1 : 1)
            {
                const auto behind = forward ? readHead - triggerAt (k, jitter) : triggerAt (k, jitter) - readHead;
                if (behind >= reach)
                    break;
                if (behind >= 0.0)
                    spawnGrain (k, (int) (behind / stepAbs));
            }
        }
        else if (std::abs (newGrid - grid) > 1.0e-9 * grid)
        {
            grid = newGrid; // DENSITY, SIZE or the speed changed: the grid goes on from here
            resync();
        }
    }
    primed = true;

    for (int n = 0; n < numSamples; ++n)
    {
        if (positional)
        {
            for (int guard = 0; guard < maxGrains && (forward ? readHead >= triggerAt (nextGrain, jitter) : readHead <= triggerAt (nextGrain, jitter)); ++guard)
            {
                spawnGrain (nextGrain, 0);
                nextGrain += forward ? 1 : -1;
            }
        }
        else if ((samplesToNextGrain -= 1.0) <= 0.0)
        {
            spawn (sample, chop, s, baseRate, 0, readHead);
            samplesToNextGrain += interval * (1.0 + jitter * (nextRandom() * 2.0 - 1.0));
        }

        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < numActive;)
        {
            auto& g = grains[(size_t) i];

            // The envelope from the block's table (no cos / exp per grain and sample).
            const auto idx = (int) g.envPhase;
            const auto env = envTable[(size_t) idx] + (g.envPhase - (float) idx) * (envTable[(size_t) idx + 1] - envTable[(size_t) idx]);
            const auto left = read (srcL, g.position);
            l += left * env * g.gainL;
            r += (mono ? left : read (srcR, g.position)) * env * g.gainR;

            g.position += g.step;
            // Wrap inside the chop so long grains on short chops never leave the region.
            if (g.position < regionStart || g.position >= regionStart + regionLen)
                g.position = regionStart + std::fmod (std::fmod (g.position - regionStart, regionLen) + regionLen, regionLen);

            g.envPhase = std::min ((float) envTableSize, g.envPhase + g.envStep);
            if (++g.age >= g.length)
            {
                // Done: the last active grain takes its place (it has not played this sample yet).
                g.active = false;
                if (i != --numActive)
                {
                    g = grains[(size_t) numActive];
                    grains[(size_t) numActive].active = false;
                }
                continue;
            }
            ++i;
        }

        outL[n] = l * norm;
        outR[n] = r * norm;

        readHead += headStep;
        if (readHead < regionStart || readHead >= regionStart + regionLen)
        {
            readHead = regionStart + std::fmod (std::fmod (readHead - regionStart, regionLen) + regionLen, regionLen);
            if (positional)
                resync(); // round again (LOOP): the grains of the new pass, from where it is
        }
    }
}

} // namespace kerf
