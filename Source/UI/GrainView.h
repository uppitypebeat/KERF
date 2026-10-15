#pragma once

#include "Controls.h"
#include <deque>
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** GRAIN view, styled like a hardware synth display (a dark glowing screen in any theme).

    Left, the GRAIN GALAXY (deep space in every theme): the chop is a glowing disc seen from
    a little above, start to end round its ring, its loudness a corona that flares where the
    grains read. The read head is a comet on the ring, SPRAY a bright arc, the core a pulsing
    star with turning arms. Live grains float above the disc at their pitch (SHAPE: square,
    round or comet) and shed sparks that drift out into the galaxy; RANDOM on a scale shows
    its pitches as orbits, SYNC sends rings out on the beat, FREEZE stills it all in ice.

    Right, CANDY: the output sound visualised: triggered oscilloscope, stereo vectorscope
    with phosphor afterglow (plus correlation), and a scrolling spectrogram. */
class GrainView final : public juce::Component, private AnimationClock::Client
{
public:
    GrainView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~GrainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Opening animation: the screens switch on one after another. */
    void playIntro() { intro = 0.0f; repaint(); }
    /** Nothing moving: the view has stopped repainting (the screenshot tool checks it). */
    bool isResting() const noexcept { return idle.quiet >= IdleGate::settleSeconds; }

private:
    static constexpr int bins = 96;   // resolution across the chop

    struct LiveGrain { float u, level, semis; };

    void animationTick (double dt) override;
    void updateBaseline();
    void updateVectorscope();
    void updateSpectrogram();

    void paintEngine (juce::Graphics&, juce::Rectangle<float> screen);
    void buildSky (juce::Rectangle<int> size);
    void paintOscilloscope (juce::Graphics&, juce::Rectangle<float> screen);
    void paintVectorscope (juce::Graphics&, juce::Rectangle<float> screen);
    void paintSpectrogram (juce::Graphics&, juce::Rectangle<float> screen);
    void paintScreen (juce::Graphics&, juce::Rectangle<float> screen, const juce::String& label) const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> header, engineScreen, oscScreen, vectorScreen, spectroScreen;

    // The galaxy: grain activity and the chop's loudness round the ring, sparks, the sky.
    struct Particle { float u = 0, radius = 1, height = 0, drift = 0, rise = 0, age = 0, life = 1, size = 1, hue = 0; };
    struct Star { float angle, radius, size, twinkle; };
    struct Meteor { juce::Point<float> from, velocity; float age = 0, life = 1; };    // in 0..1 of the screen
    struct Shockwave { float age = 0, strength = 1; };                              // a ring out over the disc on a hit
    std::array<float, bins> profile {}, baseline {};
    std::vector<Particle> particles;
    std::vector<Star> stars;
    std::vector<Meteor> meteors;
    std::vector<Shockwave> shockwaves;
    float lastLevel = 0.0f; // the grains' loudness a frame ago (a jump: a shockwave)
    float meteorClock = 0.0f;
    juce::Image skyImage;
    juce::Colour skyColour;
    bool skyLight = false; // the sky was built for white mode
    juce::Random sparkRandom;
    float starSpin = 0.0f, swirl = 0.0f;
    int shownChopId = 0, baselineChopId = -1;  // shownChopId: the voice's chop (or the whole sample)
    std::optional<ChopParams> region;        // the stretch of the sample shown (a chop)
    bool regionInSample = false;             // the whole sample plays: region = the chop it reads in
    int64_t baselineStart = -1, baselineEnd = -1;
    std::vector<LiveGrain> live;
    double sprayCentreU = 0.5, sprayHalfU = 0.0;
    float glow = 0.0f;
    float intro = 1.0f; // opening animation 0..1 (screens switch on)
    double time = 0.0;
    IdleGate idle; // rests while nothing moves

    // Candy
    std::deque<std::vector<juce::Point<float>>> vectorTraces; // newest last; normalised -1..1 (side, mid)
    juce::Image spectroImage { juce::Image::RGB, 180, 96, true, juce::SoftwareImageType() };
    juce::Colour spectroAccent; // the theme colour the history is drawn in
    float correlation = 0.0f;
    float vectorGain = 4.0f;   // auto-gain so the trace fills the scope
    int freshFrames = 0;       // frames that arrived since the last frame
    bool vectorSilent = true;
};

} // namespace kerf::ui
