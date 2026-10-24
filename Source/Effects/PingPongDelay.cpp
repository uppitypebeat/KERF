#include "PingPongDelay.h"

namespace kerf
{

void PingPongDelay::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lineSize = (int) std::ceil (maxDelaySeconds * sampleRate) + 4;
    lineL.assign ((size_t) lineSize, 0.0f);
    lineR.assign ((size_t) lineSize, 0.0f);

    smoothDelay.reset (sampleRate, 0.25); // slow glide when the tempo or division changes
    for (auto* s : { &smoothWet, &smoothDry, &smoothFeedback, &smoothWidth, &smoothEnable })
        s->reset (sampleRate, 0.03);
    smoothPingPong.reset (sampleRate, 0.08); // PING-PONG <-> STEREO: a short crossfade
    smoothPingPong.setCurrentAndTargetValue (1.0f);

    smoothDry.setCurrentAndTargetValue (1.0f);
    smoothEnable.setCurrentAndTargetValue (1.0f);
    lastTone = -1.0f;
    reset();
}

void PingPongDelay::reset()
{
    std::fill (lineL.begin(), lineL.end(), 0.0f);
    std::fill (lineR.begin(), lineR.end(), 0.0f);
    writePos = 0;
    lpStateL = lpStateR = hpStateL = hpStateR = 0.0f;
    jumpToTargetDelay = true;
}

float PingPongDelay::readDelay (const std::vector<float>& line, double delaySamples) const noexcept
{
    double readPos = (double) writePos - delaySamples;
    while (readPos < 0.0)
        readPos += lineSize;

    const auto i0 = (int) readPos;
    const auto i1 = (i0 + 1) % lineSize;
    const auto frac = (float) (readPos - i0);
    return line[(size_t) i0] + frac * (line[(size_t) i1] - line[(size_t) i0]);
}

void PingPongDelay::process (const float* sendL, const float* sendR, float* outL, float* outR, int numSamples, const Settings& s)
{
    if (lineSize == 0)
        return;

    const auto amount = juce::jlimit (0.0f, 1.0f, s.amount);
    const auto effectiveFeedback = juce::jlimit (0.0f, 0.95f, s.feedback) * (0.45f + 0.55f * amount);

    // Equal-power wet curve, compensated for the energy added by feedback.
    const auto wet = std::sin (amount * juce::MathConstants<float>::halfPi)
                     * std::sqrt (1.0f - effectiveFeedback * effectiveFeedback) * 0.9f;
    const auto dry = 1.0f - 0.2f * amount; // about -2 dB at full SPACE; the echoes make up the rest

    const auto maxDelay = (double) (lineSize - 4);
    const auto targetDelay = juce::jlimit (1.0, maxDelay, s.delaySeconds * sampleRate);

    // After prepare/reset, start every smoother at its target (no glide from zero);
    // afterwards, glide so parameter changes never click.
    const bool jump = std::exchange (jumpToTargetDelay, false);
    auto set = [jump] (auto& smoother, auto value)
    {
        if (jump) smoother.setCurrentAndTargetValue (value);
        else      smoother.setTargetValue (value);
    };

    set (smoothWet, wet);
    set (smoothDry, s.enabled ? dry : 1.0f);
    set (smoothFeedback, effectiveFeedback);
    set (smoothWidth, juce::jlimit (0.0f, 1.0f, s.width));
    set (smoothEnable, s.enabled ? 1.0f : 0.0f);
    set (smoothDelay, targetDelay);
    set (smoothPingPong, s.pingPong ? 1.0f : 0.0f);

    if (! juce::approximatelyEqual (s.tone, lastTone))
    {
        lastTone = s.tone;
        // Damping low-pass from ~1.2 kHz (dark) to ~16 kHz (bright).
        const auto cutoff = 1200.0 * std::pow (16000.0 / 1200.0, (double) juce::jlimit (0.0f, 1.0f, s.tone));
        lpCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * cutoff / sampleRate));
        // Fixed low cut (~90 Hz) keeps the repeats from getting muddy.
        hpCoeff = (float) std::exp (-juce::MathConstants<double>::twoPi * 90.0 / sampleRate);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto delay = smoothDelay.getNextValue();
        const auto fb = smoothFeedback.getNextValue();
        const auto width = smoothWidth.getNextValue();
        const auto enable = smoothEnable.getNextValue();
        const auto pp = smoothPingPong.getNextValue(); // 1 ping-pong, 0 stereo (glides between)

        const auto tapL = readDelay (lineL, delay);
        const auto tapR = readDelay (lineR, delay);

        // PING-PONG: the mono sum enters the left line only and the cross-feedback makes it
        // bounce. STEREO: each side into its own line, feeding back into itself.
        const auto mono = 0.5f * (sendL[i] + sendR[i]) * enable;
        const auto inL = pp * mono + (1.0f - pp) * sendL[i] * enable;
        const auto inR = (1.0f - pp) * sendR[i] * enable;

        // Feedback path filtering: one-pole low-pass (damping) then one-pole high-pass.
        lpStateL += lpCoeff * (pp * tapR + (1.0f - pp) * tapL - lpStateL);
        lpStateR += lpCoeff * (pp * tapL + (1.0f - pp) * tapR - lpStateR);
        hpStateL = hpCoeff * hpStateL + (1.0f - hpCoeff) * lpStateL;
        hpStateR = hpCoeff * hpStateR + (1.0f - hpCoeff) * lpStateR;
        const auto fbL = lpStateL - hpStateL;
        const auto fbR = lpStateR - hpStateR;

        // Soft saturation in the loop: transparent at normal levels, prevents runaway.
        lineL[(size_t) writePos] = std::tanh (inL + fbL * fb);
        lineR[(size_t) writePos] = std::tanh (inR + fbR * fb);

        if (++writePos >= lineSize)
            writePos = 0;

        // Width: 1 = hard ping-pong, 0 = both taps centred.
        const auto mid = 0.5f * (tapL + tapR);
        const auto wetL = mid + width * (tapL - mid);
        const auto wetR = mid + width * (tapR - mid);

        const auto w = smoothWet.getNextValue();
        const auto d = smoothDry.getNextValue();

        outL[i] = outL[i] * d + wetL * w;
        outR[i] = outR[i] * d + wetR * w;
    }
}

} // namespace kerf
