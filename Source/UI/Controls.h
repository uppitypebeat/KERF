#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace kerf::ui
{

/** Drives every UI animation from one timer owned by the editor. */
class AnimationClock
{
public:
    struct Client
    {
        virtual ~Client() = default;
        virtual void animationTick (double secondsSinceLast) = 0;
    };

    void add (Client* c)    { clients.add (c); }
    void remove (Client* c) { clients.remove (c); }
    void tick (double dt)   { clients.call ([dt] (Client& c) { c.animationTick (dt); }); }

private:
    juce::ListenerList<Client> clients;
};

/** Lets a busy view rest: it keeps repainting while something happens (sound, a parameter
    moving, the mouse, an animation running) and for a moment after, then stops until the
    next thing happens. An open editor at rest costs (almost) nothing. */
struct IdleGate
{
    /** Call once per frame; true = repaint this frame. */
    bool update (double dt, bool active, uint32_t changes) noexcept
    {
        if (active || changes != lastChanges)
            quiet = 0.0;
        else
            quiet += dt;
        lastChanges = changes;
        return quiet < settleSeconds;
    }
    void wake() noexcept { quiet = 0.0; }

    static constexpr double settleSeconds = 1.0; // long enough for glides and fades to finish
    double quiet = 0.0;
    uint32_t lastChanges = 0;
};

/** Rotary control. Derives from juce::Slider so host parameter attachments, accessibility
    and gestures work, but all drawing and mouse handling is custom.

      drag up/right   increase (cursor hidden, unlimited travel)
      shift + drag    fine adjustment
      wheel / arrows  step (shift = fine)
      double-click    reset to default (on the disc), or type a value (on the readout) */
class Knob final : public juce::Slider, private AnimationClock::Client
{
public:
    enum class Size { large, small, hero };

    Knob (const juce::String& label, Size size = Size::large, bool bipolar = false);
    ~Knob() override;

    void attachClock (AnimationClock* clock);
    void setLabel (const juce::String& l)  { label = l; repaint(); }
    void setBipolar (bool b)               { bipolar = b; repaint(); }
    void setDefaultValue (double v)        { setDoubleClickReturnValue (true, v); }

    /** Opening animation: the needle starts at its origin and sweeps to the value after
        `delaySeconds` (pages stagger their knobs). Needs an attached clock. */
    void playIntro (double delaySeconds);

    /** Called when the user starts / ends an edit (used for undo grouping of chop params). */
    std::function<void()> onGestureStart, onGestureEnd;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void valueChanged() override;
    void enablementChanged() override { repaint(); }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    static constexpr int preferredWidth (Size s)  { return s == Size::hero ? 84 : (s == Size::large ? 56 : 52); }
    static constexpr int preferredHeight (Size s) { return s == Size::hero ? 104 : (s == Size::large ? 84 : 70); }

private:
    void animationTick (double dt) override;
    juce::Rectangle<float> discArea() const;
    juce::Rectangle<float> readoutArea() const;
    /** `hold`: drag travel (of the full range) a magnetic detent keeps the knob for; 0 = none. */
    void nudge (double proportionDelta, double hold = 0.0);
    bool hasDetents() const;  // semitone / cent knobs: they click into each step
    double notchStep() const; // the marked steps in the ring (every octave on a pitch knob)
    std::vector<double> magneticValues() const; // where a drag clicks in: the default and the marks
    void stepBy (int steps);
    void showTextEntry();

    juce::String label;
    Size size;
    bool bipolar;
    AnimationClock* clock = nullptr;

    double displayProportion = 0.0; // animated needle position
    double introDelay = 0.0;        // > 0: waiting to sweep in
    bool introSweep = false;        // sweeping in (slower, eased)
    float hover = 0.0f, hoverTarget = 0.0f;
    bool dragging = false;
    juce::Point<float> lastDragPos;
    double residual = 0.0; // unsnapped drag remainder for stepped parameters
    double detentPull = 0.0; // drag travel spent pulling the knob off the detent it rests on
    std::unique_ptr<juce::TextEditor> entry;
};

/** Small rectangular text button with three states: idle, hover, on (accent). */
class PillButton final : public juce::Button
{
public:
    explicit PillButton (const juce::String& text, bool isToggle = false);

    void setAccentWhenOn (bool b) { accentWhenOn = b; repaint(); }
    void setRecordStyle (bool b)  { recordStyle = b; repaint(); }
    void setPrimary (bool b)      { primary = b; repaint(); } // filled accent: the main action in a group
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    int getIdealWidth() const;
    // Only the left button clicks (a right-click does nothing).
    void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }

private:
    bool accentWhenOn = true;
    bool recordStyle = false;
    bool primary = false;
};

/** A small square toggle with a drawn symbol instead of text (waveform tools). */
class IconButton final : public juce::Button
{
public:
    enum class Icon { magnet, knife, undo, redo, record, capture, load, plus, minus, play, stop, pause, loop, metronome, trash, resample, previous, next, menu, dice };

    /** `toggle`: stays on when clicked (tools). `recordStyle`: the record colour when on. */
    IconButton (const juce::String& name, Icon icon, bool toggle = true, bool recordStyle = false);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void setIconWhenOn (Icon i) { onIcon = i; hasOnIcon = true; repaint(); } // e.g. play -> stop while playing
    void setDangerOnHover (bool b) { dangerOnHover = b; repaint(); } // red under the mouse (a destructive action)
    std::function<void()> onRightClick; // e.g. a menu of options
    // Only the left button clicks (a right-click does nothing, or opens `onRightClick`).
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e);
        else if (onRightClick) onRightClick();
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }

private:
    Icon icon;
    bool recordStyle;
    Icon onIcon = Icon::stop;
    bool hasOnIcon = false;
    bool dangerOnHover = false;
};

/** A row of mutually exclusive text options (e.g. STR | TRI | DUP | DOT). */
class Segmented final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Segmented (juce::StringArray options, int selected = 0);

    void setSelected (int index, juce::NotificationType = juce::sendNotification);
    int getSelected() const noexcept { return selected; }
    int getNumOptions() const noexcept { return options.size(); }
    void setOptions (juce::StringArray newOptions);
    void setVertical (bool v) { vertical = v; repaint(); } // options stacked top to bottom
    void setColumns (int c)   { columns = c; repaint(); }  // a grid with this many columns (0 = one row)
    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    int indexAt (juce::Point<int>) const;
    juce::StringArray options;
    int selected = 0, hovered = -1;
    bool vertical = false;
    int columns = 0;
    juce::Rectangle<float> cellBounds (int index) const;
};

/** Numeric readout that can be dragged vertically or double-clicked to type (e.g. BPM). */
class ValueField final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ValueField (double minValue, double maxValue, double step, int decimals);

    void setValue (double v, juce::NotificationType = juce::dontSendNotification);
    double getValue() const noexcept { return value; }
    void setPlaceholder (const juce::String& p) { placeholder = p; repaint(); }
    void setHasValue (bool b) { hasValue = b; repaint(); }
    void setDimmed (bool b)   { dimmed = b; repaint(); }
    void setSuffix (const juce::String& s) { suffix = s; repaint(); }
    void setLarge (bool b)    { large = b; repaint(); }
    void setDragStep (double s) { dragStep = s; } // a coarser step for dragging (Shift: the fine step)
    void setJustification (juce::Justification j) { justification = j; repaint(); }
    std::function<juce::String (double)> formatter; // optional custom display text
    std::function<void (double)> onChange;
    std::function<void()> onPopupMenu; // right-click (optional)

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override; // one (drag) step per notch

private:
    double value = 0.0, minV, maxV, step;
    double dragStep = 0.0;
    int decimals;
    bool hasValue = true, dimmed = false, large = false, dragging = false;
    juce::Justification justification = juce::Justification::centredLeft;
    juce::String placeholder { "--" }, suffix;
    float dragStartY = 0.0f;
    double dragStartValue = 0.0;
    std::unique_ptr<juce::TextEditor> entry;
};

/** A horizontal slider with its zero in the middle (SHIFT): drag left or right (Shift: fine),
    the wheel nudges one step (Shift: a tenth), a double-click goes back to 0. The value is
    written next to it. `onDragEnd`: a drag finished (one undo step). */
class CentreSlider final : public juce::Component, public juce::SettableTooltipClient
{
public:
    CentreSlider (double range, double step, int decimals, juce::String suffix);

    void setValue (double v, juce::NotificationType = juce::dontSendNotification);
    double getValue() const noexcept { return value; }
    std::function<void (double)> onChange;
    std::function<void()> onDragEnd;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<float> trackArea() const;
    double value = 0.0, range, step;
    int decimals;
    juce::String suffix;
    bool dragging = false;
    float lastX = 0.0f;
    double dragValue = 0.0; // unrounded while dragging (fine steps add up)
};

/** Thin vertical/horizontal separator lines used to group sections. */
void drawHairline (juce::Graphics& g, juce::Rectangle<float> line, juce::Colour c = Colours::hairline);

/** Uppercase section heading with an optional context tag on the right (e.g. "CHOP 03"). */
void drawSectionHeader (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title,
                        const juce::String& tag = {}, bool tagIsAccent = false);

} // namespace kerf::ui
