#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace kerf
{

/** Real-time spectrum for the UI.

    Audio thread: push() copies a mono mix into a lock-free FIFO (never blocks; if the UI
    falls behind, samples are dropped). UI thread: update() runs the FFT on the newest
    samples and produces smoothed, log-frequency-spaced band levels in dB. */
class SpectrumAnalyzer
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int numBands = 96;
    static constexpr int numFineBands = 360; // the EQ backdrop

    SpectrumAnalyzer();

    void setSampleRate (double rate) noexcept { sampleRate.store (rate); }

    /** Audio thread. */
    void push (const float* left, const float* right, int numSamples) noexcept;

    /** UI thread. Returns true if new data was processed. */
    bool update();

    /** Smoothed band levels in dB (-90..0), UI thread only. */
    const std::array<float, numBands>& getBands() const noexcept { return bands; }

    /** Band centre frequency (log-spaced 30 Hz .. 18 kHz). */
    static float bandFrequency (int band) noexcept;

    /** A finer, power-averaged spectrum (dB, -90..6) for large displays: detailed but
        without the spikes of the peak-picked bands. UI thread only. */
    const std::array<float, numFineBands>& getFineBands() const noexcept { return fineBands; }

    /** Fine band centre frequency (log-spaced 20 Hz .. 20 kHz). */
    static float fineBandFrequency (int band) noexcept;

private:
    juce::AbstractFifo fifo { 16384 };
    std::vector<float> fifoData;
    std::atomic<double> sampleRate { 44100.0 };

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris, true };
    std::vector<float> history; // last fftSize samples
    std::vector<float> fftData;
    std::array<float, numBands> bands {};
    std::array<float, numFineBands> fineBands {};
};

} // namespace kerf
