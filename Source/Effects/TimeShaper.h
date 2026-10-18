#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>

namespace kerf
{

/** A curve over one TIME cycle (x 0..1), piecewise linear; two points at the same x make
    a jump. TIME curves: y = how far behind real time playback reads, as a fraction of the
    cycle (0 = live). VOLUME curves: y = gain 0..1. */
struct TimeCurve
{
    static constexpr int maxPoints = 64;
    int numPoints = 0;
    std::array<float, maxPoints> x {}, y {};

    float valueAt (float position) const noexcept;
    /** The slope of the segment at `position` (jumps are skipped: they have no slope). */
    float slopeAt (float position) const noexcept;
    void add (float px, float py) noexcept { if (numPoints < maxPoints) { x[(size_t) numPoints] = px; y[(size_t) numPoints] = py; ++numPoints; } }
    void sort() noexcept; // by x, keeping the order of points at the same x
    juce::String toString() const;
    static TimeCurve fromString (const juce::String&);
    static TimeCurve flat (float value) { TimeCurve c; c.add (0.0f, value); c.add (1.0f, value); return c; }
    bool operator== (const TimeCurve& o) const noexcept;
};

/** One TIME slot: what playback does over a cycle (time) and how loud (volume). */
struct TimeSlot
{
    TimeCurve time = TimeCurve::flat (0.0f), volume = TimeCurve::flat (1.0f);
};

namespace TimeFactory
{
    constexpr int numSlots = 60;     // three banks of 20: A tempo and tape, B repeats and stutters, C gates and volume
    constexpr int slotsPerBank = 20;
    /** The factory shapes in page order: bank A tempo and tape (HALF TIME ... REV HALF), bank B
        repeats and stutters (STUTTER 3 ... STUTTER FADE), bank C gates and volume (GATE 16 ...
        PUMP 4). Every slot can be drawn over (it keeps your curves until reset). */
    const std::array<TimeSlot, numSlots>& slots();
    const char* name (int slot);
}

/** TIME: bends the output in time like a tape under your hand. The last seconds of audio
    are kept; each sample is read from as far back as the slot's TIME curve says at this
    point of the cycle (a cycle is 1/2, 1 or 2 bars, locked to the beat), and scaled by its
    VOLUME curve. Slower reading is half time or a tape stop, backwards is reverse, jumps
    are stutters and repeats (crossfaded over 6 ms, no clicks). It can only read the past:
    a lag builds up and is let go at the next jump. Real-time safe. */
class TimeShaper
{
public:
    struct Settings
    {
        const TimeSlot* slot = nullptr; // nullptr = off (the history keeps filling)
        float mix = 1.0f;
        double cycleBeats = 4.0;
        // The display only: is the song (or the pattern preview) playing? Stopped, the screen shows
        // `stoppedBeat` (where the host stands) while the audio clock runs on for live playing.
        bool clockRunning = true;
        double stoppedBeat = 0.0;
    };

    void prepare (double sampleRate);
    void reset();
    /** `ppq`: the beat at the first sample; `bpm`: the tempo the cycle follows. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& settings, double ppq, double bpm) noexcept;

    // For the UI (any thread): where in the cycle, how fast the tape runs, how far behind.
    float getDisplayPosition() const noexcept { return displayPosition.load (std::memory_order_relaxed); }
    float getDisplaySpeed() const noexcept    { return displaySpeed.load (std::memory_order_relaxed); }
    float getDisplayLagBeats() const noexcept { return displayLag.load (std::memory_order_relaxed); }
    bool isEngaged() const noexcept           { return displayEngaged.load (std::memory_order_relaxed); }
    bool isClockRunning() const noexcept      { return displayRunning.load (std::memory_order_relaxed); }

    static constexpr double historySeconds = 12.0; // 2 bars down to 40 BPM (slower: the lag is clamped)

private:
    float read (int channel, double delaySamples) const noexcept;

    double rate = 48000.0;
    juce::AudioBuffer<float> history;
    int writePos = 0, size = 1;
    double lastDelay = 0.0, fadeDelay = 0.0;
    float fade = 0.0f, fadeStep = 0.0f;
    float gain = 1.0f, mix = 0.0f, smoothing = 0.0f;
    std::atomic<float> displayPosition { 0.0f }, displaySpeed { 1.0f }, displayLag { 0.0f };
    std::atomic<bool> displayEngaged { false }, displayRunning { false };
};

} // namespace kerf
