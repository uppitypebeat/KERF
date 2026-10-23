#include "GrainHaze.h"

namespace kerf
{

namespace
{
    // Line lengths (ms) with no common factors, so the echoes never line up into a pitch.
    constexpr double lineMs[] = { 37.1, 41.3, 47.9, 53.3, 61.7, 67.9, 73.1, 83.3 };
    constexpr double lfoHz[] = { 0.13, 0.17, 0.23, 0.29, 0.31, 0.37, 0.41, 0.47 };
    constexpr double diffuserMs[] = { 4.77, 3.59, 12.73, 9.31 };
    constexpr double maxSize = 1.5;     // the room grows with HAZE up to this
    constexpr double modulationMs = 0.6;
}

void GrainHaze::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (int i = 0; i < numLines; ++i)
    {
        auto& l = lines[(size_t) i];
        l.baseDelay = lineMs[i] * 0.001 * rate;
        l.buffer.assign ((size_t) std::ceil ((lineMs[i] + modulationMs * 2.0) * maxSize * 0.001 * rate) + 8, 0.0f);
        l.lfoStep = juce::MathConstants<double>::twoPi * lfoHz[i] / rate;
        l.lfoPhase = (double) i * 0.785; // spread out
    }
    for (int i = 0; i < numDiffusers; ++i)
        diffusers[(size_t) i].buffer.assign ((size_t) std::max (1, (int) std::lround (diffuserMs[i] * 0.001 * rate)), 0.0f);
    reset();
}

void GrainHaze::reset()
{
    for (auto& l : lines)
    {
        std::fill (l.buffer.begin(), l.buffer.end(), 0.0f);
        l.write = 0;
        l.damp = 0.0f;
    }
    for (auto& d : diffusers)
    {
        std::fill (d.buffer.begin(), d.buffer.end(), 0.0f);
        d.write = 0;
    }
    highPassL = highPassR = 0.0f;
    level = 0.0f;
    quietSamples = 0;
}

void GrainHaze::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, float amount)
{
    juce::ScopedNoDenormals noDenormals;
    if (lines[0].buffer.empty())
        return;
    const auto target = juce::jlimit (0.0f, 1.0f, amount);

    // Nothing to do: HAZE at zero, or silence going in with the tail long gone.
    float peakIn = 0.0f;
    for (int n = 0; n < numSamples; ++n)
        peakIn = std::max (peakIn, std::max (std::abs (inL[n]), std::abs (inR[n])));
    if (target <= 0.0f && shownAmount < 1.0e-4f)
    {
        shownAmount = 0.0f;
        if (level > 0.0f)
            reset(); // (its output is silent at zero anyway; start clean next time)
        return;
    }
    quietSamples = peakIn < 1.0e-6f && level < 1.0e-6f ? quietSamples + numSamples : 0;
    if (quietSamples > (int) rate)
        return;

    // The room for this block: HAZE sets its size, its decay and how dark the tail gets.
    const auto a = (double) juce::jlimit (0.0f, 1.0f, std::max (target, shownAmount));
    const auto size = 1.0 + (maxSize - 1.0) * a;
    const auto rt60 = 2.5 + 9.5 * a * a;
    const auto dampCoeff = (float) std::exp (-juce::MathConstants<double>::twoPi * (7000.0 - 3500.0 * a) / rate);
    const auto highPass = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 150.0 / rate));
    const auto depth = modulationMs * 0.001 * rate * (0.5 + a);
    std::array<float, numLines> feedback {};
    for (int i = 0; i < numLines; ++i)
        feedback[(size_t) i] = (float) std::pow (10.0, -3.0 * lines[(size_t) i].baseDelay * size / (rt60 * rate));
    const auto smoothing = (float) (1.0 - std::exp (-1.0 / (0.05 * rate)));
    const auto levelSmoothing = (float) (1.0 - std::exp (-1.0 / (0.2 * rate)));
    constexpr float diffusion = 0.65f, inputGain = 0.35f;

    for (int n = 0; n < numSamples; ++n)
    {
        shownAmount += (target - shownAmount) * smoothing;
        const auto wet = std::pow (shownAmount, 0.8f) * 0.9f;

        // In: the lows taken out; mid (diffused) and side (for width).
        highPassL += (inL[n] - highPassL) * highPass;
        highPassR += (inR[n] - highPassR) * highPass;
        const auto l = inL[n] - highPassL, r = inR[n] - highPassR;
        auto mid = (l + r) * 0.5f;
        const auto side = (l - r) * 0.5f;
        for (auto& d : diffusers)
        {
            const auto delayed = d.buffer[(size_t) d.write];
            const auto out = delayed - diffusion * mid;
            d.buffer[(size_t) d.write] = mid + diffusion * out;
            d.write = (d.write + 1) % (int) d.buffer.size();
            mid = out;
        }

        // The lines: read (modulated, damped), mix, write back with the decay.
        std::array<float, numLines> y {};
        float sum = 0.0f;
        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[(size_t) i];
            const auto size_ = (int) line.buffer.size();
            const auto delay = line.baseDelay * size + depth * std::sin (line.lfoPhase);
            line.lfoPhase += line.lfoStep;
            if (line.lfoPhase > juce::MathConstants<double>::twoPi)
                line.lfoPhase -= juce::MathConstants<double>::twoPi;
            auto readPos = (double) line.write - delay;
            while (readPos < 0.0)
                readPos += size_;
            const auto i0 = (int) readPos;
            const auto frac = (float) (readPos - (double) i0);
            const auto s0 = line.buffer[(size_t) i0], s1 = line.buffer[(size_t) ((i0 + 1) % size_)];
            const auto v = s0 + (s1 - s0) * frac;
            line.damp = v + (line.damp - v) * dampCoeff;
            y[(size_t) i] = line.damp;
            sum += line.damp;
        }
        const auto householder = sum * (2.0f / (float) numLines);
        float outLeft = 0.0f, outRight = 0.0f;
        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[(size_t) i];
            const auto sign = (i & 2) ? -1.0f : 1.0f;
            const auto in = (mid * sign + side * (i < numLines / 2 ? 1.0f : -1.0f)) * inputGain;
            line.buffer[(size_t) line.write] = (y[(size_t) i] - householder) * feedback[(size_t) i] + in;
            line.write = (line.write + 1) % (int) line.buffer.size();
            if (i % 2 == 0) outLeft += y[(size_t) i] * sign;
            else            outRight += y[(size_t) i] * sign;
        }

        outL[n] += outLeft * 1.4f * wet;
        outR[n] += outRight * 1.4f * wet;
        level += ((std::abs (outLeft) + std::abs (outRight)) * 0.7f * wet - level) * levelSmoothing;
    }
}

} // namespace kerf
