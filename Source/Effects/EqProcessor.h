#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <complex>

namespace kerf
{

/** A parametric EQ with up to eight points (bell, shelves, cuts, notch).

    Coefficients are computed in place (RBJ cookbook biquads), so changing a setting never
    allocates on the audio thread. Settings glide over ~30 ms (coefficients refreshed every
    32 samples), and points fade in and out when they are added or removed, so editing
    never clicks. With no point doing anything the EQ is bypassed bit-exactly. */
class EqProcessor
{
public:
    static constexpr int maxBands = 8;

    enum class Type { bell = 0, lowShelf, highShelf, lowCut, highCut, notch };

    struct Band
    {
        bool on = false;
        bool bypass = false; // there, but not heard (right-click on the point)
        Type type = Type::bell;
        float freq = 1000.0f, gainDb = 0.0f, q = 0.9f;

        /** True when the band changes the sound (cuts and notches always do). */
        bool isAudible() const noexcept
        {
            if (! on || bypass) return false;
            return type == Type::lowCut || type == Type::highCut || type == Type::notch || std::abs (gainDb) >= 0.01f;
        }
        bool usesGain() const noexcept { return type == Type::bell || type == Type::lowShelf || type == Type::highShelf; }
    };

    struct Settings
    {
        std::array<Band, maxBands> bands {};

        bool isFlat() const noexcept
        {
            for (const auto& b : bands)
                if (b.isAudible())
                    return false;
            return true;
        }
        /** What is heard: bypassed points switched off (they fade out like a removed one). */
        Settings heard() const noexcept
        {
            auto s = *this;
            for (auto& b : s.bands)
                b.on = b.on && ! b.bypass;
            return s;
        }
    };

    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

        static Biquad lowShelf (double sampleRate, double freq, double gainDb) noexcept;
        static Biquad peak (double sampleRate, double freq, double q, double gainDb) noexcept;
        static Biquad highShelf (double sampleRate, double freq, double gainDb) noexcept;
        static Biquad lowCut (double sampleRate, double freq, double q) noexcept;
        static Biquad highCut (double sampleRate, double freq, double q) noexcept;
        static Biquad notch (double sampleRate, double freq, double q) noexcept;
        static Biquad forBand (const Band& band, double sampleRate) noexcept;

        /** Magnitude response in dB at `freq` (used by the EQ display). */
        double magnitudeDb (double freq, double sampleRate) const noexcept;
    };

    /** Combined response of all active points, in dB (for drawing the curve). */
    static double responseDb (const Settings& s, double freq, double sampleRate = 48000.0) noexcept;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s);

private:
    double sampleRate = 44100.0;
    std::array<Biquad, maxBands> filters {};
    std::array<std::array<float, 2>, maxBands * 2> state {}; // [band * 2 + channel] = { z1, z2 }
    std::array<float, maxBands> mix {};                       // 0..1: fades a point in / out
    Settings current;
    bool active = false;
};

} // namespace kerf
