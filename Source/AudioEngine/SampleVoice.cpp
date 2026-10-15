#include "SampleVoice.h"

namespace kerf
{

namespace
{
    constexpr double declickSeconds   = 0.0015; // minimum ramp at chop edges
    constexpr double killSeconds      = 0.004;  // voice steal / choke fade
    constexpr double loopXfadeSeconds = 0.006;  // crossfade at loop wrap
    constexpr float velocityRangeDb   = 18.0f;  // velocity 0 -> -18 dB, 127 -> 0 dB
}

void SampleVoice::prepare (double hostSampleRate)
{
    sampleRate = hostSampleRate;
    killStep = (float) (1.0 / std::max (1.0, killSeconds * hostSampleRate));

    for (auto* s : { &smoothGain, &smoothPanL, &smoothPanR, &smoothSpace, &smoothGrain })
        s->reset (hostSampleRate, 0.02);

    cloud.prepare (hostSampleRate);

    stage = Stage::idle;
    sample = nullptr;
}

void SampleVoice::start (const SampleData::Ptr& newSample, const ChopParams& newChop, int midiNote, float velocity, uint64_t newAge,
                         const SampleData::Ptr& prerendered, bool playOnce, float transposeSemitones, double from)
{
    if (newSample == nullptr || newChop.length() <= 0)
        return;

    sample = newSample;
    chop = newChop;
    forcedOneShot = playOnce;
    forcedLoop = false;
    stepSemitones = transposeSemitones;
    glideTarget = transposeSemitones;
    glidePerSample = 0.0f;
    if (forcedOneShot)
        chop.mode = forcedMode();
    isPrerendered = false;
    originalStart = newChop.start;
    originalLength = newChop.length();

    if (prerendered != nullptr && prerendered->getNumSamples() > 0)
    {
        // The render already contains this chop's pitch and the tempo ratio: play the
        // whole render at its natural rate.
        sample = prerendered;
        isPrerendered = true;
        chop.start = 0;
        chop.end = prerendered->getNumSamples();
        chop.pitchSemitones = 0.0f;
        chop.fineCents = 0.0f;
    }
    note = midiNote;
    age = newAge;
    stage = Stage::playing;
    envelope = 0.0f;
    attackDone = false;

    velocityGain = juce::Decibels::decibelsToGain (-velocityRangeDb * (1.0f - juce::jlimit (0.0f, 1.0f, velocity)));

    const auto attackSeconds = std::max (declickSeconds, (double) chop.fadeInMs * 0.001);
    attackStep = (float) (1.0 / std::max (1.0, attackSeconds * sampleRate));
    const auto releaseSeconds = std::max (declickSeconds, (double) chop.fadeOutMs * 0.001);
    releaseStep = (float) (1.0 / std::max (1.0, releaseSeconds * sampleRate));
    computeDecay();

    position = chop.reverse ? (double) chop.end - 1.0 : (double) chop.start;
    if (from > (double) originalStart && from < (double) (originalStart + originalLength))
    {
        // Started from a frame inside the chop (the playhead put there): the same place in a
        // render too. Reversed, it plays from there back to the start.
        const auto fraction = (from - (double) originalStart) / (double) originalLength;
        position = (double) chop.start + fraction * (double) chop.length();
    }

    gainLinear = juce::Decibels::decibelsToGain (chop.gainDb, ChopLimits::minGainDb) * velocityGain;
    smoothGain.setCurrentAndTargetValue (gainLinear);
    computePanGains (chop.pan);
    smoothPanL.setCurrentAndTargetValue (smoothPanL.getTargetValue());
    smoothPanR.setCurrentAndTargetValue (smoothPanR.getTargetValue());
    smoothSpace.setCurrentAndTargetValue (chop.spaceSend);
    smoothGrain.setCurrentAndTargetValue (-1.0f); // snapped to the real amount on the first render
    cloud.start (chop, position); // grains from where the voice starts (the same there every time)
    beatLine = noBeatLine;
}

void SampleVoice::noteOff()
{
    if (stage == Stage::playing && chop.mode != PlayMode::oneShot)
        stage = Stage::releasing;
}

void SampleVoice::kill()
{
    if (stage != Stage::idle)
        stage = Stage::killing;
}

void SampleVoice::updateChop (const ChopParams& updated)
{
    if (stage == Stage::idle || updated.id != chop.id)
        return;

    if (isPrerendered)
    {
        // Keep playing the render we started with; only take the non-timing settings.
        const auto keepStart = chop.start, keepEnd = chop.end;
        chop = updated;
        chop.start = keepStart;
        chop.end = keepEnd;
        chop.pitchSemitones = 0.0f;
        chop.fineCents = 0.0f;
    }
    else
    {
        chop = updated;
    }

    if (forcedOneShot)
        chop.mode = forcedMode();

    position = juce::jlimit ((double) chop.start, (double) std::max (chop.start, chop.end - 1), position);

    const auto releaseSeconds = std::max (declickSeconds, (double) chop.fadeOutMs * 0.001);
    releaseStep = (float) (1.0 / std::max (1.0, releaseSeconds * sampleRate));
    computeDecay();

    gainLinear = juce::Decibels::decibelsToGain (chop.gainDb, ChopLimits::minGainDb) * velocityGain;
    smoothGain.setTargetValue (gainLinear);
    computePanGains (chop.pan);
    smoothSpace.setTargetValue (chop.spaceSend);
}

void SampleVoice::computeDecay() noexcept
{
    // 1 % of the distance to the sustain level is left after decayMs.
    const auto samples = std::max (1.0, (double) juce::jlimit (ChopLimits::minDecayMs, ChopLimits::maxDecayMs, chop.decayMs) * 0.001 * sampleRate);
    decayCoeff = (float) std::exp (std::log (0.01) / samples);
}

void SampleVoice::computePanGains (float panValue) noexcept
{
    // Balance law: centre = unity on both sides, so a centred chop plays at its true level.
    smoothPanL.setTargetValue (std::min (1.0f, 1.0f - panValue));
    smoothPanR.setTargetValue (std::min (1.0f, 1.0f + panValue));
}

float SampleVoice::readInterpolated (const float* data, double pos) const noexcept
{
    // 4-point, 3rd-order Hermite interpolation.
    const auto total = (int64_t) sample->getNumSamples();
    const auto i = (int64_t) std::floor (pos);
    const auto f = (float) (pos - (double) i);

    auto at = [data, total] (int64_t idx) noexcept { return data[juce::jlimit<int64_t> (0, total - 1, idx)]; };

    const float x0 = at (i - 1), x1 = at (i), x2 = at (i + 1), x3 = at (i + 2);
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * f + c2) * f + c1) * f + x1;
}

double SampleVoice::readRate (const VoiceGlobals& g) const
{
    return isPrerendered
               ? semitonesToRatio (stepSemitones) * sample->sampleRate / std::max (1.0, g.hostSampleRate)
               : semitonesToRatio (totalSemitones (chop, g.globalSemitones) + stepSemitones)
                     * g.timeRatio * sample->sampleRate / std::max (1.0, g.hostSampleRate);
}

double SampleVoice::framesUntilEnd (const VoiceGlobals& g) const
{
    if (stage == Stage::idle || sample == nullptr)
        return 0.0;
    const auto remainingSource = chop.reverse ? (position - (double) chop.start + 1.0) : ((double) chop.end - position);
    return std::max (0.0, remainingSource / std::max (1.0e-9, readRate (g)));
}

void SampleVoice::noteBeatLine (int offset, double stepFrames, const VoiceGlobals& g)
{
    // Where the read position is in the original sample, in beats from its start (the beat
    // lines the waveform draws). A line passed clicks; the place where the voice starts (or
    // a loop starts again) clicks only when it is right on a line.
    const auto stepInOriginal = std::max (1.0e-9, std::abs (toDisplay (position + stepFrames) - toDisplay (position)));
    const auto beats = (toDisplay (position) - g.beatOrigin) / g.beatFrames;
    const auto line = (int64_t) std::floor (beats + 1.0e-9);
    auto click = [&] (int64_t which)
    {
        auto& c = *g.beatClicks;
        if (c.count < (int) c.offset.size())
        {
            c.offset[(size_t) c.count] = offset;
            c.downbeat[(size_t) c.count] = ((which % g.beatsPerBar) + g.beatsPerBar) % g.beatsPerBar == 0;
            ++c.count;
        }
    };
    const auto rightOnLine = (beats - (double) line) * g.beatFrames < stepInOriginal;

    const auto wrapped = beatLine != noBeatLine && (chop.reverse ? line > beatLine : line < beatLine);
    if (beatLine == noBeatLine || wrapped)
    {
        if (rightOnLine)
            click (line);
    }
    else if (line != beatLine)
    {
        click (std::max (line, beatLine)); // forwards: the new line; backwards: the one passed
    }
    beatLine = line;
}

void SampleVoice::render (const VoiceBusses& out, int startSample, int numSamples, const VoiceGlobals& g)
{
    if (stage == Stage::idle || sample == nullptr || sample->getNumSamples() == 0)
        return;

    const float* srcL = sample->audio.getReadPointer (0);
    const float* srcR = sample->audio.getReadPointer (sample->getNumChannels() > 1 ? 1 : 0);

    // Pre-rendered chops already contain pitch and tempo: only sample-rate conversion.
    // A pattern step's pitch moves even a pre-rendered chop (by resampling it).
    const double rate = readRate (g);
    // Slide: the pitch moves on during this block; the read speed follows it sample by sample.
    double stepDelta = 0.0;
    int glideSamples = 0; // samples of this block the glide still moves
    if (glidePerSample != 0.0f)
    {
        glideSamples = std::min (numSamples, (int) std::ceil ((glideTarget - stepSemitones) / glidePerSample));
        auto next = stepSemitones + glidePerSample * (float) glideSamples;
        if (glideSamples < numSamples || (glidePerSample > 0.0f && next >= glideTarget) || (glidePerSample < 0.0f && next <= glideTarget))
        {
            next = glideTarget; // arrives in this block
            glidePerSample = 0.0f;
        }
        const auto rateEnd = rate * semitonesToRatio ((double) (next - stepSemitones));
        stepDelta = (rateEnd - rate) / std::max (1, glideSamples) * (chop.reverse ? -1.0 : 1.0);
        stepSemitones = next;
    }
    double step = chop.reverse ? -rate : rate;

    const double regionStart = (double) chop.start;
    const double regionEnd   = (double) chop.end;
    const double regionLen   = regionEnd - regionStart;
    const bool looping = chop.mode == PlayMode::loop && stage == Stage::playing;

    // Fade near the end of the chop (in output samples) so one-shots and gates never click
    // at the boundary. Uses the chop's fade-out time, but never more than half the chop.
    const double chopOutputLength = regionLen / std::max (1e-9, rate);
    const double endFadeOut = juce::jlimit (declickSeconds * sampleRate,
                                            std::max (declickSeconds * sampleRate, chopOutputLength * 0.5),
                                            (double) chop.fadeOutMs * 0.001 * sampleRate);

    const double loopXfade = std::min (loopXfadeSeconds * sample->sampleRate, regionLen * 0.25);

    // --- GRAIN: render the cloud for this block, then crossfade it in per sample ---------
    const auto grainAmount = juce::jlimit (0.0f, 1.0f, g.grain.mix * chop.grainSend);
    if (smoothGrain.getTargetValue() < 0.0f)
        smoothGrain.setCurrentAndTargetValue (grainAmount);
    else
        smoothGrain.setTargetValue (grainAmount);

    const bool grainsOn = (grainAmount > 0.0005f || smoothGrain.isSmoothing()) && out.scratchL != nullptr;
    if (grainsOn)
        cloud.render (*sample, chop, g.grain, rate, out.scratchL + startSample, out.scratchR + startSample, numSamples);

    for (int n = 0; n < numSamples; ++n)
    {
        // --- Envelope -------------------------------------------------------
        // Attack up to full level, then decay towards the sustain level (exponentially: 1 %
        // of the way left after the decay time), release on note-off.
        switch (stage)
        {
            case Stage::playing:
                if (! attackDone)
                {
                    envelope = std::min (1.0f, envelope + attackStep);
                    attackDone = envelope >= 1.0f;
                }
                else if (envelope > chop.sustain)
                {
                    envelope = chop.sustain + (envelope - chop.sustain) * decayCoeff;
                    if (envelope - chop.sustain < 1.0e-4f)
                        envelope = chop.sustain;
                }
                break;
            case Stage::releasing: envelope -= releaseStep; break;
            case Stage::killing:   envelope -= killStep; break;
            case Stage::idle:      break;
        }

        // Done when faded out - also while held, once a sustain of 0 has decayed away.
        if ((envelope <= 0.0f && stage != Stage::playing) || (attackDone && chop.sustain <= 0.0f && envelope < 3.0e-4f))
        {
            stage = Stage::idle;
            break;
        }

        // --- Position / end of region --------------------------------------
        const double remainingSource = chop.reverse ? (position - regionStart + 1.0) : (regionEnd - position);

        if (remainingSource <= 0.0)
        {
            if (looping)
            {
                position += chop.reverse ? regionLen : -regionLen;
            }
            else
            {
                stage = Stage::idle;
                break;
            }
        }

        // --- Metronome: the sample's beat lines this voice passes ------------
        if (g.beatClicks != nullptr && g.beatFrames > 0.0 && forcedOneShot && stage == Stage::playing)
            noteBeatLine (n + startSample, std::abs (step), g);

        // --- Read sample (with loop crossfade near the wrap point) ----------
        float l = readInterpolated (srcL, position);
        float r = srcL == srcR ? l : readInterpolated (srcR, position);

        float endGain = 1.0f;

        if (looping)
        {
            const double distToWrap = chop.reverse ? (position - regionStart) : (regionEnd - position);
            if (distToWrap < loopXfade && loopXfade > 1.0)
            {
                const auto t = (float) (1.0 - distToWrap / loopXfade); // 0 -> 1 approaching wrap
                const double other = position + (chop.reverse ? regionLen : -regionLen);
                const float ol = readInterpolated (srcL, other);
                const float orr = srcL == srcR ? ol : readInterpolated (srcR, other);
                // Equal-power crossfade.
                const float a = std::cos (t * juce::MathConstants<float>::halfPi);
                const float b = std::sin (t * juce::MathConstants<float>::halfPi);
                l = l * a + ol * b;
                r = r * a + orr * b;
            }
        }
        else
        {
            const double remainingOut = remainingSource / std::max (1e-9, rate);
            if (remainingOut < endFadeOut)
                endGain = (float) juce::jlimit (0.0, 1.0, remainingOut / endFadeOut);
        }

        const int idx = startSample + n;

        // --- Grain crossfade (equal power) ------------------------------------
        const float grain = smoothGrain.getNextValue();
        if (grainsOn)
        {
            const float dryGain = std::cos (grain * juce::MathConstants<float>::halfPi);
            const float wetGain = std::sin (grain * juce::MathConstants<float>::halfPi);
            l = l * dryGain + out.scratchL[idx] * wetGain;
            r = r * dryGain + out.scratchR[idx] * wetGain;
        }

        // --- Gain, pan, output ---------------------------------------------
        const float amp = envelope * endGain * smoothGain.getNextValue();
        const float outL = l * amp * smoothPanL.getNextValue();
        const float outR = r * amp * smoothPanR.getNextValue();
        const float send = smoothSpace.getNextValue();

        out.dryL[idx] += outL;
        out.dryR[idx] += outR;

        if (out.spaceL != nullptr)
        {
            out.spaceL[idx] += outL * send;
            out.spaceR[idx] += outR * send;
        }

        position += step;
        if (n < glideSamples)
            step += stepDelta;
    }
}

} // namespace kerf

namespace kerf
{

void SampleVoice::applyPad (const PadSound& pad) noexcept
{
    if (stage == Stage::idle)
        return;

    auto c = chop;
    pad.applyTo (c);
    if (isPrerendered)
    {
        // Pitch is baked into a STRETCH render (a new render follows automatically).
        c.pitchSemitones = 0.0f;
        c.fineCents = 0.0f;
    }
    if (forcedOneShot)
        c.mode = forcedMode();

    const bool same = c.pitchSemitones == chop.pitchSemitones && c.fineCents == chop.fineCents && c.gainDb == chop.gainDb
                   && c.pan == chop.pan && c.fadeInMs == chop.fadeInMs && c.fadeOutMs == chop.fadeOutMs
                   && c.decayMs == chop.decayMs && c.sustain == chop.sustain
                   && c.spaceSend == chop.spaceSend && c.grainSend == chop.grainSend && c.reverse == chop.reverse && c.mode == chop.mode;
    if (! same)
        updateChop (c);
}

} // namespace kerf
