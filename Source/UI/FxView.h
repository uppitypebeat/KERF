#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** FX view, in the same synth-display style as GRANULIZER (dark glowing screens).

    Left, FX CHAIN: a 3D landscape of the output spectrum (low to high across, time going
    back to the horizon). Every effect that is on changes it:
      SPACE   ghost echoes of the front ridge, bouncing left / right (ping-pong)
      DIRT    WARM saturates (soft, hot glow), FUZZ clips (flat tops, sparks), CRUSH steps
      TONE    a wall of light at the cutoff; what it removes is darkened; resonance peak
      WOBBLE  the landscape wavers (TAPE: slow and uneven, CHORUS: even, doubled image)
      PUMP    the landscape breathes with the pump: dims on the dip, glows on the swell,
              and a pulse rolls back over it on every beat (or 8th, 16th)

    Right: one small screen per effect with its own live animation: a ball bouncing between
    the speakers (ping-pong echoes), transfer curve, filter response, LFO trace, and the
    pump's level curve over a bar with the beat running along it. */
class FxView final : public juce::Component, private AnimationClock::Client
{
public:
    FxView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~FxView() override;

    /** Opening animation: the screens switch on one after another. */
    void playIntro() { intro = 0.0f; repaint(); }
    /** Nothing moving: the view has stopped repainting (the screenshot tool checks it). */
    bool isResting() const noexcept { return idle.quiet >= IdleGate::settleSeconds; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int bins = 96;   // = SpectrumAnalyzer::numBands
    static constexpr int rows = 28;   // depth of the landscape

    struct Settings
    {
        float space = 0.0f, dirt = 0.0f, filter = 0.0f, resonance = 0.0f, wobble = 0.0f, pump = 0.0f;
        int dirtMode = 0, wobbleMode = 0, pumpMode = 0;
        double delaySeconds = 0.25;
        float feedback = 0.0f;
    };

    void animationTick (double dt) override;
    Settings readSettings() const;
    float shapeHeight (float h, int bin) const; // DIRT applied to a ridge height

    void paintChain (juce::Graphics&, juce::Rectangle<float> screen);
    void paintSpace (juce::Graphics&, juce::Rectangle<float> screen);
    void paintDirt (juce::Graphics&, juce::Rectangle<float> screen);
    void paintTone (juce::Graphics&, juce::Rectangle<float> screen);
    void paintWobble (juce::Graphics&, juce::Rectangle<float> screen);
    void paintPump (juce::Graphics&, juce::Rectangle<float> screen);
    void moduleFrame (juce::Graphics&, juce::Rectangle<float> screen, const juce::String& label,
                      const juce::String& value, bool on) const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> header, chainScreen, spaceScreen, dirtScreen, toneScreen, wobbleScreen, pumpScreen;

    std::array<std::array<float, bins>, rows> history {};
    std::array<float, bins> current {};
    int newestRow = 0, tickCount = 0;
    Settings s;
    juce::Random random { 0x46582121 };
    float level = 0.0f;   // output level (smoothed)
    float sample = 0.0f;  // latest output sample (for the DIRT transfer dot)
    double time = 0.0;
    double motion = 0.0;   // WOBBLE's clock: runs only while something plays
    IdleGate idle; // rests while nothing moves
    double pumpPpq = 0.0;  // the beat the PUMP follows
    float pumpGain = 1.0f; // its level right now (0..1)
    float intro = 1.0f;
};

} // namespace kerf::ui
