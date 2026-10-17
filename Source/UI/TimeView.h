#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** The slot shown and edited on the TIME page (the active one, or the last chosen). */
int timeEditSlot (KerfAudioProcessor& processor);
void setTimeEditSlot (KerfAudioProcessor& processor, int slot);

/** TIME view, a synth display like GRAIN and FX.

    Left: the slot's TIME curve (top: live, down: further behind; segments coloured by how
    fast the tape runs there: normal, slower, backwards) and its VOLUME curve under it, both
    editable: click to add a point, drag to move it, double-click to remove it, right-click
    for more; snapped to the grid (Shift: free). A beam follows the beat.

    Right, TAPE: two reels turning at the speed playback runs (slowing to a stop, running
    backwards), and SPEED, the last seconds of that speed as a scrolling trace. */
class TimeView final : public juce::Component, private AnimationClock::Client
{
public:
    TimeView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~TimeView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** Opening animation: the screens switch on, the curves draw themselves. */
    void playIntro() { intro = 0.0f; repaint(); }
    /** Nothing moving: the view has stopped repainting (the screenshot tool checks it). */
    bool isResting() const noexcept { return idle.quiet >= IdleGate::settleSeconds; }
    /** Steps the animation by hand (the screenshot tool). */
    void advance (double seconds) { animationTick (seconds); }

private:
    enum Lane { timeLane = 0, volumeLane = 1 };
    struct Hit { int lane = -1, index = -1; };

    void animationTick (double dt) override;
    juce::Rectangle<float> plotArea (int lane) const;
    juce::Point<float> toScreen (int lane, float x, float y) const;
    juce::Point<float> fromScreen (int lane, juce::Point<float> p, bool snap) const;
    Hit hitTest (juce::Point<float> p) const;
    int laneAt (juce::Point<float> p) const;
    TimeCurve& curveOf (TimeSlot& slot, int lane) const { return lane == timeLane ? slot.time : slot.volume; }
    void showMenu (int lane);
    /** After an edit: the shown slot changed, and is switched on when TIME was off (you hear what you draw). */
    void store (const TimeSlot& slot);
    juce::String slotTitle() const;   // "SLOT 01  HALF TIME  (EDITED)" in the header
    void placeReset();

    void paintCurveScreen (juce::Graphics&, int lane);
    void paintTape (juce::Graphics&, juce::Rectangle<float> screen);
    void paintSpeed (juce::Graphics&, juce::Rectangle<float> screen);

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> header, timeScreen, volumeScreen, tapeScreen, speedScreen;
    TimeSlot shown;                      // the slot being shown (refreshed every frame)
    int shownSlot = -1;
    Hit hover, drag;
    float intro = 1.0f, beam = 0.0f, running = 0.0f, tapeRun = 0.0f, speed = 1.0f, reelAngle = 0.0f, reelAngleRight = 0.0f, engaged = 0.0f;
    float lagRange = 0.5f;               // how far behind the TIME screen reaches (fraction of the cycle)
    float lagTarget = 0.5f;
    std::array<float, 240> speedHistory {}; // ~4 s at 60 frames a second
    int speedWrite = 0;
    double time = 0.0;
    PillButton reset { "Reset" };      // in the header after the slot's name: back to its factory shape
    juce::String placedFor;            // the title the reset button was placed after
    IdleGate idle; // rests while nothing moves
};

/** The TIME slots as cards (a mini picture of each curve). Click: switch that slot on; the
    active one again: the TIME page back to how it started. Right-click: a menu. */
class TimeSlots final : public juce::Component, private AnimationClock::Client
{
public:
    TimeSlots (KerfAudioProcessor& processor, AnimationClock& clock);
    ~TimeSlots() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    /** Which bank of 20 is shown (A, B, C). */
    void setBank (int b) { bank = juce::jlimit (0, TimeFactory::numSlots / TimeFactory::slotsPerBank - 1, b); hovered = -1; repaint(); }
    int getBank() const noexcept { return bank; }
    std::function<void (int)> onBankChanged; // it switched by itself (a slot in another bank came on)

    void mouseDown (const juce::MouseEvent&) override;
    /** A click on slot `k`: on (and shown); the active one again: the TIME page reset. */
    void clickSlot (int k);

private:
    void animationTick (double dt) override;
    juce::Rectangle<float> cardBounds (int slot) const;
    int cardAt (juce::Point<float> p) const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    int hovered = -1, shownActive = -2, shownEdit = -2;
    int bank = 0;
    std::array<float, TimeFactory::numSlots> glow {};
    juce::String lastSignature;
};

} // namespace kerf::ui
