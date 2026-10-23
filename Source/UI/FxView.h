#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** FX view: one big screen, the WORMHOLE. You fly through a tunnel of rings; each ring is
    the output spectrum at the moment it was born (lows at the bottom, highs at the top, the
    same on both sides), born in the light at the far end and rushing past you. Stars streak
    by at the speed of the flight. Every effect has its own, clearly visible part:

      ECHO    every ring drags ghost rings behind it, thrown left and right (ping-pong)
      DIRT    WARM: the rings glow hot and thick. FUZZ: flat-topped, crackling rings that
              spray sparks. CRUSH: the rings break into hard-edged polygons
      FILTER  a string of lights runs down both walls at the cutoff; what the filter removes
              goes dark (low-pass: the top of the tunnel, high-pass: the bottom)
      WOBBLE  the tunnel bends and snakes (TAPE: slow and uneven, CHORUS: even, and every
              ring doubled)
      PUMP    the tunnel contracts on every pump, the light at the end flashes and the flight
              surges */
class FxView final : public juce::Component, private AnimationClock::Client
{
public:
    FxView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~FxView() override;

    /** Opening animation: the screen switches on. */
    void playIntro() { intro = 0.0f; repaint(); }
    /** Nothing moving (the screenshot tool checks it). */
    bool isResting() const noexcept { return idle.quiet >= IdleGate::settleSeconds; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int bins = 96;      // = SpectrumAnalyzer::numBands
    static constexpr int points = 96;    // points round a ring
    static constexpr int maxRings = 34;

    struct Settings
    {
        float space = 0.0f, dirt = 0.0f, filter = 0.0f, resonance = 0.0f, wobble = 0.0f, pump = 0.0f;
        int dirtMode = 0, wobbleMode = 0, pumpMode = 0;
        double delaySeconds = 0.25;
        float feedback = 0.0f;
    };
    struct Ring { std::array<float, bins> spectrum {}; float depth = 1.0f; };   // depth: 0 here .. 1 at the far end
    struct Star { float angle = 0.0f, depth = 1.0f, speed = 1.0f, size = 1.0f; };
    struct Spark { juce::Point<float> pos, velocity; float age = 0.0f, life = 0.5f; };

    void animationTick (double dt) override;
    Settings readSettings() const;
    float shapeHeight (float h) const;   // DIRT applied to a ring's height
    float heightAt (const Ring& ring, float u) const; // the ring's spectrum at u (0 bottom .. 1 top)

    // The tunnel's geometry: where the ring at `depth` is centred (WOBBLE bends the tunnel)
    // and how large it is (perspective).
    juce::Point<float> centreAt (float depth) const;
    float radiusAt (float depth) const;
    juce::Path ringPath (const Ring& ring, float depth, juce::Point<float> offset, bool passBand, bool& any) const;

    void paintTunnel (juce::Graphics&, juce::Rectangle<float> screen);

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> header, tunnelScreen;

    std::array<float, bins> current {};
    std::vector<Ring> rings;
    std::vector<Star> stars;
    std::vector<Spark> sparks;
    Settings s;
    juce::Random random { 0x46582121 };
    float level = 0.0f;      // output level (smoothed)
    float flash = 0.0f;      // PUMP's flash at the far end
    float speed = 0.0f;      // the flight's speed (depth per second)
    float spawn = 0.0f;      // distance flown since the last ring
    double time = 0.0;
    double motion = 0.0;     // WOBBLE's clock: runs only while something plays (it goes on from where it stood)
    IdleGate idle;
    double pumpPpq = 0.0;    // the beat the PUMP follows
    float pumpGain = 1.0f;   // its level right now (0..1)
    float intro = 1.0f;
};

} // namespace kerf::ui
