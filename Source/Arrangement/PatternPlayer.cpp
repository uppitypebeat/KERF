#include "PatternPlayer.h"

namespace kerf
{

namespace
{
    /** Deterministic noise in -1..1 for a step (and a stream, so timing and velocity differ). */
    float stepNoise (int64_t step, uint32_t stream) noexcept
    {
        auto x = (uint32_t) (step * 2654435761u) ^ (stream * 0x9e3779b9u);
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return (float) (x & 0xffffff) / (float) 0x7fffff - 1.0f;
    }
}

double PatternPlayer::grooveTime (int64_t step, const Groove& g) noexcept
{
    auto t = (double) step;
    if ((step & 1) != 0)
        t += 0.5 * juce::jlimit (0.0, 1.0, (double) g.swing);                    // the off-16th, later
    t += 0.12 * juce::jlimit (0.0, 1.0, (double) g.humanize) * stepNoise (step, 1); // a little early or late
    return t;
}

float PatternPlayer::grooveVelocity (int64_t step, float velocity, const Groove& g) noexcept
{
    const auto h = juce::jlimit (0.0f, 1.0f, g.humanize);
    return juce::jlimit (0.05f, 1.0f, velocity * (1.0f + 0.2f * h * stepNoise (step, 2)));
}

bool PatternPlayer::stepPlays (int64_t step, float chance) noexcept
{
    if (chance >= 0.999f)
        return true;
    return 0.5f * (stepNoise (step, 3) + 1.0f) < chance; // uniform 0..1 against the chance
}

int PatternPlayer::hitsFor (const PatternEvent& e, int64_t step, const Groove& groove, std::array<Hit, PatternEvent::maxRepeats>& hits) noexcept
{
    if (! stepPlays (step, e.chance))
        return 0;

    const auto repeats = juce::jlimit (1, PatternEvent::maxRepeats, e.repeats);
    const auto start = grooveTime (step, groove);
    const auto end = (double) step + (double) std::max (1, e.length); // the next step's grid line
    const auto each = (end - start) / repeats;
    const auto velocity = grooveVelocity (step, e.velocity, groove);

    for (int k = 0; k < repeats; ++k)
    {
        hits[(size_t) k].on = start + k * each;
        hits[(size_t) k].end = start + (k + 1) * each;
        hits[(size_t) k].off = hits[(size_t) k].end - std::min (0.02, each * 0.25); // released just before the next hit
        hits[(size_t) k].velocity = velocity;
    }
    return repeats;
}

int PatternPlayer::noteFor (const PatternData& p, int i, int64_t step) noexcept
{
    const auto& e = p.events[(size_t) i];
    const auto own = p.notes[(size_t) i];
    if (e.random == 0 || own < 0 || p.numChopNotes <= 0)
        return own;

    const auto roll = 0.5f * (stepNoise (step, 4) + 1.0f); // 0..1, its own stream
    if (e.random == 2)
        return p.chopNotes[(size_t) juce::jlimit (0, p.numChopNotes - 1, (int) (roll * (float) p.numChopNotes))];

    const auto centre = juce::jlimit (0, p.numChopNotes - 1, p.chopIndex[(size_t) i]);
    const auto lo = std::max (0, centre - 2), hi = std::min (p.numChopNotes - 1, centre + 2);
    return p.chopNotes[(size_t) (lo + juce::jlimit (0, hi - lo, (int) (roll * (float) (hi - lo + 1))))];
}

void PatternPlayer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    for (auto& a : active)
        a.note = -1;
    queued = 0;
    expectedNextStep = -1.0;
    wasRunning = false;
    currentStep.store (-1);
}

void PatternPlayer::allNotesOff (juce::MidiBuffer& out)
{
    for (auto& a : active)
        if (a.note >= 0)
        {
            out.addEvent (juce::MidiMessage::noteOff (midiChannel, a.note), 0);
            a.note = -1;
        }
    queued = 0;
}

void PatternPlayer::releaseDue (double beforeStep, const BlockClock& clock, juce::MidiBuffer& out)
{
    for (auto& a : active)
        if (a.note >= 0 && a.offStep < beforeStep)
        {
            out.addEvent (juce::MidiMessage::noteOff (midiChannel, a.note), clock.offsetFor (std::max (a.offStep, clock.stepStart)));
            a.note = -1;
        }
}

void PatternPlayer::process (const PatternData& pattern, bool running, double ppqAtStart, double bpm, int numSamples,
                             juce::MidiBuffer& out, const Groove& groove)
{
    if (! running || pattern.numSteps <= 0 || bpm <= 0.0)
    {
        if (wasRunning)
            allNotesOff (out);
        wasRunning = false;
        queued = 0;
        expectedNextStep = -1.0;
        currentStep.store (-1, std::memory_order_relaxed);
        return;
    }

    const auto stepsPerSample = bpm / 60.0 * 4.0 / sampleRate;
    const auto stepStart = ppqAtStart * 4.0;
    const auto stepEnd = stepStart + numSamples * stepsPerSample;
    const BlockClock clock { stepStart, stepsPerSample, numSamples };

    // Discontinuity (loop, seek, tempo jump): release everything before continuing.
    if (wasRunning && expectedNextStep >= 0.0 && std::abs (stepStart - expectedNextStep) > 0.25)
    {
        allNotesOff (out);
        lastEmittedStep = -1.0e9;
    }
    if (! wasRunning)
        lastEmittedStep = -1.0e9;

    wasRunning = true;
    expectedNextStep = stepEnd;

    // Schedule the hits of every step whose (grooved) start falls inside the block. Swing
    // moves a step up to half a step later and humanize at most 0.12 steps either way, so
    // the steps to look at start a little before the block. Repeats of a step may fall
    // into later blocks; they wait in the queue.
    for (auto k = std::floor (stepStart - 0.7); k < stepEnd + 0.2; k += 1.0)
    {
        const auto step = (int64_t) k;
        const auto at = grooveTime (step, groove);
        if (at < stepStart || at >= stepEnd || k <= lastEmittedStep)
            continue; // not in this block, or already played at the end of the previous one
        lastEmittedStep = k;

        const auto patternStep = (step % pattern.numSteps + pattern.numSteps) % pattern.numSteps;
        currentStep.store ((int) patternStep, std::memory_order_relaxed);

        for (int i = 0; i < pattern.numEvents; ++i)
        {
            const auto& e = pattern.events[(size_t) i];
            if (e.step != patternStep)
                continue;
            const auto note = noteFor (pattern, i, step);
            if (note < 0)
                continue;

            // Slide: the event before this one (in pattern order, round the loop) slides into
            // it - this one starts at that pitch and glides to its own in half a step.
            const auto& previous = pattern.events[(size_t) ((i + pattern.numEvents - 1) % pattern.numEvents)];
            const auto glide = pattern.numEvents > 1 && previous.slide;
            const auto glideTens = juce::jlimit (1, 127, (int) std::lround (0.5 * 60.0 / bpm / 4.0 * 100.0));

            std::array<Hit, PatternEvent::maxRepeats> hits;
            const auto n = hitsFor (e, step, groove, hits);
            for (int h = 0; h < n && queued < (int) queue.size(); ++h)
                queue[(size_t) queued++] = { note, e.pitch, hits[(size_t) h].velocity, hits[(size_t) h].on, hits[(size_t) h].off,
                                             previous.pitch, glideTens, glide && h == 0 };
        }
    }

    // Play the queued hits that are due, in time order; note-offs due before each one first.
    for (;;)
    {
        int next = -1;
        for (int q = 0; q < queued; ++q)
            if (queue[(size_t) q].on < stepEnd && (next < 0 || queue[(size_t) q].on < queue[(size_t) next].on))
                next = q;
        if (next < 0)
            break;

        const auto hit = queue[(size_t) next];
        queue[(size_t) next] = queue[(size_t) --queued];

        releaseDue (hit.on, clock, out);
        const auto offset = clock.offsetFor (std::max (hit.on, stepStart));

        // Retrigger: end a previous instance of the same note first.
        for (auto& a : active)
            if (a.note == hit.note)
            {
                out.addEvent (juce::MidiMessage::noteOff (midiChannel, a.note), offset);
                a.note = -1;
            }

        if (hit.glide)
        {
            out.addEvent (juce::MidiMessage::controllerEvent (midiChannel, glideFromController, juce::jlimit (0, 127, 64 + hit.glideFrom)), offset);
            out.addEvent (juce::MidiMessage::controllerEvent (midiChannel, glideTimeController, hit.glideTens), offset);
        }
        if (hit.pitch != 0)
            out.addEvent (juce::MidiMessage::pitchWheel (midiChannel, pitchToWheel (hit.pitch)), offset);
        out.addEvent (juce::MidiMessage::noteOn (midiChannel, hit.note, hit.velocity), offset);

        for (auto& a : active)
            if (a.note < 0)
            {
                a.note = hit.note;
                a.offStep = hit.off;
                break;
            }
    }

    // Notes ending inside this block.
    releaseDue (stepEnd, clock, out);

    if (currentStep.load (std::memory_order_relaxed) < 0)
        currentStep.store ((int) (((int64_t) std::floor (stepStart) % pattern.numSteps + pattern.numSteps) % pattern.numSteps), std::memory_order_relaxed);
}

} // namespace kerf
