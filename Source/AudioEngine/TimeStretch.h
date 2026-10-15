#pragma once

#include <cmath>
#include <functional>
#include "SampleData.h"

namespace kerf
{

/** Offline, pitch-independent time-stretching of one chop (Signalsmith Stretch, MIT).

    Runs on a background thread. The result is a new SampleData containing only the
    chop, already stretched to `timeRatio` (output length = input length / timeRatio)
    and transposed by `semitones`, so the audio thread can play it back at rate 1.0 with
    no extra latency and no real-time DSP cost.

    Short chops are rendered with up to 150 ms of the surrounding audio on both sides
    (the stretcher needs context), then cropped back to the chop exactly. */
namespace TimeStretch
{
    struct Request
    {
        int64_t start = 0, end = 0;
        double semitones = 0.0;
        double timeRatio = 1.0; // > 1 = faster / shorter

        bool operator== (const Request& o) const noexcept
        {
            return start == o.start && end == o.end
                && std::abs (semitones - o.semitones) < 1.0e-4
                && std::abs (timeRatio - o.timeRatio) < 1.0e-4;
        }
        bool operator!= (const Request& o) const noexcept { return ! (*this == o); }
    };

    /** Returns nullptr if cancelled or if the request is invalid. */
    SampleData::Ptr render (const SampleData& source, const Request& request,
                            const std::function<bool()>& cancelled = {});

    /** True when the chop needs no processing (ratio 1, no transposition). */
    inline bool isIdentity (const Request& r) noexcept
    {
        return std::abs (r.timeRatio - 1.0) < 1.0e-4 && std::abs (r.semitones) < 1.0e-4;
    }
}

} // namespace kerf
