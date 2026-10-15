#include "ChopGrid.h"

#include <algorithm>
#include <cmath>

namespace kerf
{

double beatsForNoteValue (NoteValue v) noexcept
{
    switch (v)
    {
        case NoteValue::whole:        return 4.0;
        case NoteValue::half:         return 2.0;
        case NoteValue::quarter:      return 1.0;
        case NoteValue::eighth:       return 0.5;
        case NoteValue::sixteenth:    return 0.25;
        case NoteValue::thirtySecond: return 0.125;
    }
    return 1.0;
}

double factorForModifier (NoteModifier m) noexcept
{
    switch (m)
    {
        case NoteModifier::straight:   return 1.0;
        case NoteModifier::triplet:    return 2.0 / 3.0;
        case NoteModifier::duplet:     return 3.0 / 2.0;
        case NoteModifier::dotted:     return 3.0 / 2.0;
        case NoteModifier::quintuplet: return 4.0 / 5.0;
    }
    return 1.0;
}

double gridStepBeats (NoteValue v, NoteModifier m) noexcept
{
    return beatsForNoteValue (v) * factorForModifier (m);
}

double gridStepSamples (NoteValue v, NoteModifier m, double bpm, double sampleRate) noexcept
{
    if (! (bpm > 0.0) || ! (sampleRate > 0.0) || ! std::isfinite (bpm) || ! std::isfinite (sampleRate))
        return 0.0;

    return gridStepBeats (v, m) * (60.0 / bpm) * sampleRate;
}

std::vector<int64_t> rhythmicGrid (int64_t regionStart, int64_t regionEnd,
                                   double bpm, double sampleRate,
                                   NoteValue v, NoteModifier m,
                                   int maxSlices, double minStepSamples)
{
    if (regionEnd < regionStart)
        std::swap (regionStart, regionEnd);

    std::vector<int64_t> result { regionStart };

    const auto step = gridStepSamples (v, m, bpm, sampleRate);

    if (step >= minStepSamples && regionEnd > regionStart)
    {
        // Positions are computed from the anchor each time (not accumulated) so that
        // rounding error cannot drift across a long sample.
        for (int i = 1; i < maxSlices; ++i)
        {
            const auto pos = regionStart + (int64_t) std::llround (step * i);

            // Do not leave a sliver smaller than a quarter step at the end.
            if (pos >= regionEnd - (int64_t) (step * 0.25))
                break;

            result.push_back (pos);
        }
    }

    result.push_back (regionEnd);

    if (result.size() == 2 && result[0] == result[1])
        result.pop_back();

    return result;
}

std::vector<int64_t> equalSlices (int64_t regionStart, int64_t regionEnd, int count)
{
    if (regionEnd < regionStart)
        std::swap (regionStart, regionEnd);

    count = std::clamp (count, 1, 1024);
    const auto length = regionEnd - regionStart;
    count = (int) std::min<int64_t> (count, std::max<int64_t> (1, length));

    std::vector<int64_t> result;
    result.reserve ((size_t) count + 1);

    for (int i = 0; i <= count; ++i)
        result.push_back (regionStart + (int64_t) std::llround ((double) length * i / count));

    return result;
}

int64_t snapToGrid (int64_t position, int64_t anchor, double stepSamples) noexcept
{
    if (! (stepSamples >= 1.0) || ! std::isfinite (stepSamples))
        return position;

    const auto steps = std::round ((double) (position - anchor) / stepSamples);
    return anchor + (int64_t) std::llround (steps * stepSamples);
}

} // namespace kerf
