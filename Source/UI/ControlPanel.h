#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"
#include "TimeView.h"

namespace kerf::ui
{

/** The lower control area, one page per main window (the tabs are PageTabs, at the top):
      CHOP:  SAMPLE | ENVELOPE | PITCH | OUTPUT      (the selected chop)
      GRAIN: CLOUD | MOTION | PITCH | COLOUR
      FX:    ECHO | DIRT | WOBBLE | FILTER | PUMP    (global effects)
      EQ:    no panel: the EQ view is edited directly (drag, wheel, right-click)
    Every control is visible on its page (no hidden drawer). Sections are sized to their
    content and the spare width is shared evenly, so nothing floats in empty space. */
class ControlPanel final : public juce::Component,
                           private juce::ValueTree::Listener,
                           private juce::ChangeListener
{
public:
    ControlPanel (KerfAudioProcessor& processor, AnimationClock& clock);
    ~ControlPanel() override;

    static constexpr int panelHeight = 176;
    /** The EQ page has no panel (everything is on the EQ view: drag, wheel, right-click). */
    static int heightForPage (int pg) { return pg == 3 ? 0 : panelHeight; }

    /** 0 = CHOP, 1 = FX, 2 = GRAIN, 3 = EQ, 4 = TIME. */
    void setPage (int newPage);
    std::function<void (int)> onPageChanged; // the editor swaps the waveform for the GRAIN / EQ view
    static constexpr int numPages = 5;
    int getPage() const noexcept { return page; }

    void paint (juce::Graphics&) override;
    void resized() override;

    /** True when a page's effects do something (a dot on its tab). */
    bool isPageActive (int pg) const;
    /** A double-click on the tab switches a page off (GRANULIZER, FX, EQ, TIME; not CHOP):
        its settings stay, it just stops sounding. */
    bool isPageOff (int pg) const;
    void togglePageOff (int pg);

    /** Opening animation: the page's knobs sweep in, one after another. */
    void playPageIntro();

private:
    struct ChopBinding
    {
        enum class Kind { param, start, length };
        Knob* knob;
        juce::Identifier id;
        Kind kind;
    };

    struct Section
    {
        juce::String title;
        std::function<juce::String()> tag;
        int page = 0;
        std::vector<juce::Component*> components;
        int knobColumns = 0;       // knobs in the widest row (their spacing can grow)
        float fixedWidth = 0.0f;   // width that does not scale (hero knob, selectors)
        std::function<void (juce::Rectangle<float> content, int knobPitch)> layout;
        bool fill = false;         // takes all the width the other sections leave (TIME's slots)
        juce::Rectangle<float> area, content;
    };

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { syncFromModel(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { syncFromModel(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              {}
    void changeListenerCallback (juce::ChangeBroadcaster*) override                    { syncFromModel(); }

    Knob& addKnob (Knob& k, double defaultValue);
    void bindChop (Knob& k, const juce::Identifier& id, ChopBinding::Kind kind = ChopBinding::Kind::param);
    void attachParam (Knob& k, const char* paramId);
    void attachChoice (Segmented& s, const char* paramId);
    void applyChopValue (const ChopBinding& b, double value);
    void syncFromModel();
    juce::String chopTag() const;
    void rebindPads();

    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> padAttachments;
    int boundChopId = -1, boundSlot = -2;
    bool padBound = false;
    int selectedChopId() const;
    std::vector<int> otherSelectedChops() const; // chosen with Ctrl besides the one shown: edits apply to them too
    void updateVisibility();

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    bool syncing = false;
    bool editingChops = false; // a chop knob is in your hand (its edits go to every selected chop)
    int page = 0;

    // SAMPLE and ENVELOPE (per chop)
    Knob start { "Start" }, length { "Length" };
    Knob fadeIn { "Attack" }, decay { "Decay" }, sustain { "Sustain" }, fadeOut { "Release" };
    Segmented mode { { "ONE", "GATE", "LOOP" }, 1 };
    PillButton reverse { "Reverse", true };

    // PITCH
    Knob tune { "Tune", Knob::Size::large, true }, fine { "Fine", Knob::Size::large, true };
    Knob globalPitch { "Global", Knob::Size::large, true };
    PillButton sync { "Sync", true };
    Segmented stretchMode { { "REPITCH", "STRETCH" } };

    // OUTPUT (per chop + master)
    Knob gain { "Gain" }, pan { "Pan", Knob::Size::large, true }, master { "Master" };
    Segmented choke { { "OFF", "1", "2", "3", "4" } }; // per chop: chops in the same group cut each other off
    juce::Rectangle<float> chokeLabel;

    // SPACE
    Knob space { "Echo", Knob::Size::hero };
    Knob spaceSend { "Send", Knob::Size::small };

    // GRAIN (its own page and view)
    Knob grainMix { "Grains" }, grainDensity { "Density" }, grainSize { "Size" }, grainShape { "Shape" };
    PillButton grainSync { "Sync", true }, grainFreeze { "Freeze", true };
    Segmented grainScale { { "FREE", "OCT", "5TH", "CHORD" } };
    Knob grainScan { "Speed" }, grainPosition { "Position" }, grainSpray { "Spray" }, grainDrift { "Drift" };
    Knob grainPitchShift { "Pitch", Knob::Size::large, true }, grainPitch { "Random" }, grainShimmer { "Shimmer" };
    Knob grainDetune { "Detune" }, grainGlide { "Glide", Knob::Size::large, true }, grainHaze { "Haze" };
    Knob grainSpread { "Spread" }, grainReverse { "Reverse" }, grainSend { "Chop Amt" };


    // DIRT, WOBBLE
    Knob dirt { "Dirt", Knob::Size::hero };
    Segmented dirtMode { { "WARM", "FUZZ", "CRUSH" } };
    Knob wobble { "Wobble", Knob::Size::hero };
    Segmented wobbleMode { { "TAPE", "CHORUS" } };
    Knob pump { "Pump", Knob::Size::hero };
    Segmented pumpMode { { "1/4", "1/8", "1/16" } };

    // TONE
    Knob filter { "Filter", Knob::Size::hero, true }, resonance { "Res", Knob::Size::hero };

    // TIME
    TimeSlots timeSlots;
    Knob timeMix { "Mix", Knob::Size::hero };
    Segmented timeLength { { "1/2 BAR", "1 BAR", "2 BARS" } };
    Segmented timeBank { { "A", "B", "C" } }; // which 20 TIME slots are shown
    juce::Rectangle<float> lengthLabel;

    std::vector<ChopBinding> chopBindings;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::ButtonParameterAttachment>> buttonAttachments;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> choiceAttachments;
    std::vector<Section> sections;
};

/** The four main windows as tabs at the top: CHOP, GRANULIZER, FX, EQ. A dot marks a page
    whose effects are on. Drives ControlPanel::setPage (which the editor follows). */
class PageTabs final : public juce::Component, private AnimationClock::Client
{
public:
    PageTabs (ControlPanel& panel, AnimationClock& clock);
    ~PageTabs() override;

    static constexpr int tabGap = 4;
    static juce::String pageName (int page); // CHOP, FX, GRANULIZER, EQ, TIME
    int idealWidth() const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void animationTick (double) override;
    juce::Rectangle<float> tabBounds (int page) const;
    int tabAt (juce::Point<float>) const;

    ControlPanel& panel;
    AnimationClock& clock;
    int shownPage = -1, hovered = -1;
    // A page goes off only on a quick double-click (two clicks within quickDoubleClickMs on
    // the same tab), stricter than the system's double-click time: no accidents.
    static constexpr double quickDoubleClickMs = 280.0;
    juce::Time lastDown, previousDown;
    int lastDownTab = -1, previousDownTab = -1;
    std::array<bool, ControlPanel::numPages> active {}, off {};
};

/** Over a page that is switched off (a double-click on its tab): its view and its controls
    look switched off - a dark veil with fine diagonal lines, a light sweeping across as it
    goes off, and in the middle what is off and how to switch it on. Clicks pass through
    (the settings can still be changed). Rests when it has faded in. */
class PageOffVeil final : public juce::Component, private AnimationClock::Client
{
public:
    PageOffVeil (ControlPanel& panel, AnimationClock& clock);
    ~PageOffVeil() override;

    void paint (juce::Graphics&) override;

private:
    void animationTick (double dt) override;

    ControlPanel& panel;
    AnimationClock& clock;
    int page = -1;
    float amount = 0.0f; // 0 = on (invisible) .. 1 = off
    float sweep = 1.0f;  // the light crossing once as the page goes off
};

} // namespace kerf::ui
