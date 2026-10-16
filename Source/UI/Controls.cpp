#include "Controls.h"

namespace kerf::ui
{

namespace
{
    constexpr float arcStart = -0.75f * juce::MathConstants<float>::pi;
    constexpr float arcEnd = 0.75f * juce::MathConstants<float>::pi;

    void approach (double& value, double target, double dt, double timeConstant)
    {
        const auto k = 1.0 - std::exp (-dt / std::max (1.0e-4, timeConstant));
        value += (target - value) * k;
        if (std::abs (target - value) < 1.0e-4)
            value = target;
    }
}

//==============================================================================
Knob::Knob (const juce::String& l, Size s, bool bip)
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox),
      label (l), size (s), bipolar (bip)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setTitle (l);
    displayProportion = valueToProportionOfLength (getValue());
}

Knob::~Knob()
{
    if (clock != nullptr)
        clock->remove (this);
}

void Knob::attachClock (AnimationClock* c)
{
    if (clock != nullptr)
        clock->remove (this);
    clock = c;
    if (clock != nullptr)
        clock->add (this);
}

juce::Rectangle<float> Knob::discArea() const
{
    const auto d = size == Size::hero ? 58.0f : (size == Size::large ? 40.0f : 30.0f);
    return juce::Rectangle<float> (d, d).withCentre ({ getWidth() * 0.5f, 8.0f + d * 0.5f });
}

juce::Rectangle<float> Knob::readoutArea() const
{
    const auto disc = discArea();
    return { 0.0f, disc.getBottom() + 22.0f, (float) getWidth(), 14.0f };
}

void Knob::paint (juce::Graphics& g)
{
    const auto enabled = isEnabled();
    const auto alpha = enabled ? 1.0f : 0.35f;
    const auto disc = discArea();
    const auto centre = disc.getCentre();
    const auto radius = disc.getWidth() * 0.5f;
    const auto arcRadius = radius + 5.0f;

    const auto proportion = (float) juce::jlimit (0.0, 1.0, displayProportion);
    const auto angle = arcStart + proportion * (arcEnd - arcStart);
    const auto originProportion = bipolar ? 0.5f : 0.0f;
    const auto originAngle = arcStart + originProportion * (arcEnd - arcStart);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, arcStart, arcEnd, true);
    g.setColour (Colours::hairline.withMultipliedAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (Metrics::strokeBold, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    // Value arc (from the origin: start for unipolar, centre for bipolar)
    if (std::abs (angle - originAngle) > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                std::min (originAngle, angle), std::max (originAngle, angle), true);
        g.setColour (Colours::accent.withMultipliedAlpha (alpha));
        g.strokePath (valueArc, juce::PathStrokeType (Metrics::strokeBold, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Stepped knobs (semitones, cents): notches in the ring at the musical steps (every
    // octave on a pitch knob), so the knob reads as one that clicks into place.
    if (hasDetents())
    {
        const auto every = notchStep();
        g.setColour (Colours::background.withMultipliedAlpha (alpha));
        for (auto v = getMinimum(); v <= getMaximum() + 1.0e-9; v += every)
        {
            const auto a = arcStart + (float) valueToProportionOfLength (v) * (arcEnd - arcStart);
            const juce::Point<float> d (std::sin (a), -std::cos (a));
            g.drawLine ({ centre + d * (arcRadius - 2.2f), centre + d * (arcRadius + 2.2f) }, 1.4f);
        }
    }

    // Bipolar centre tick
    if (bipolar)
    {
        g.setColour (Colours::line.withMultipliedAlpha (alpha));
        g.fillRect (juce::Rectangle<float> (1.0f, 3.0f).withCentre ({ centre.x, centre.y - arcRadius - 4.0f }));
    }

    // Disc
    const auto ringColour = Colours::line.interpolatedWith (Colours::textDim, hover * 0.5f);
    g.setColour (Colours::surfaceRaised.withMultipliedAlpha (alpha));
    g.fillEllipse (disc);
    g.setColour ((hasKeyboardFocus (false) ? Colours::accent.withAlpha (0.7f) : ringColour).withMultipliedAlpha (alpha));
    g.drawEllipse (disc.reduced (0.5f), Metrics::hairline);

    // Needle
    const auto inner = radius * 0.32f, outer = radius * 0.82f;
    const juce::Point<float> dir (std::sin (angle), -std::cos (angle));
    g.setColour (Colours::text.withMultipliedAlpha (alpha));
    g.drawLine ({ centre + dir * inner, centre + dir * outer }, Metrics::strokeBold);

    // Label and value
    const auto labelArea = juce::Rectangle<float> (0.0f, disc.getBottom() + 8.0f, (float) getWidth(), 12.0f);
    drawText (g, label.toUpperCase(), labelArea, TextStyle::label, Colours::textDim.withMultipliedAlpha (alpha), juce::Justification::centred);

    if (entry == nullptr)
    {
        const auto text = enabled ? getTextFromValue (getValue()) : juce::String ("--");
        drawText (g, text, readoutArea(), TextStyle::value,
                  (dragging ? Colours::accent : Colours::text).withMultipliedAlpha (alpha), juce::Justification::centred);
    }
}

void Knob::playIntro (double delaySeconds)
{
    if (clock == nullptr)
        return;
    displayProportion = bipolar ? 0.5 : 0.0;
    introDelay = std::max (0.0, delaySeconds);
    introSweep = true;
    repaint();
}

void Knob::animationTick (double dt)
{
    const auto target = valueToProportionOfLength (getValue());
    bool changed = false;

    if (introDelay > 0.0)
    {
        introDelay -= dt; // hold at the origin until this knob's turn
    }
    else if (std::abs (displayProportion - target) > 1.0e-5)
    {
        approach (displayProportion, target, dt, introSweep ? Motion::standard : Motion::fast * 0.6);
        changed = true;
    }
    else
    {
        introSweep = false;
    }

    if (std::abs (hover - hoverTarget) > 0.001f)
    {
        auto h = (double) hover;
        approach (h, hoverTarget, dt, Motion::fast);
        hover = (float) h;
        changed = true;
    }

    if (changed)
        repaint();
}

void Knob::valueChanged()
{
    if (dragging || clock == nullptr)
        displayProportion = valueToProportionOfLength (getValue());
    repaint();
}

void Knob::mouseEnter (const juce::MouseEvent&) { hoverTarget = 1.0f; if (clock == nullptr) { hover = 1.0f; repaint(); } }
void Knob::mouseExit (const juce::MouseEvent&)  { hoverTarget = 0.0f; if (clock == nullptr) { hover = 0.0f; repaint(); } }

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;

    lastDragPos = e.position;
    dragging = false;
    residual = 0.0;
    rawValid = false;
    if (onGestureStart) onGestureStart();
    juce::Slider::mouseDown (e); // starts the host gesture
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;

    if (! dragging)
    {
        dragging = true;
        e.source.enableUnboundedMouseMovement (true, false);
    }

    const auto delta = (e.position.x - lastDragPos.x) - (e.position.y - lastDragPos.y);
    lastDragPos = e.position;

    // Stepped knobs need more travel per step: each step clicks in instead of sliding past.
    const auto steps = hasDetents() ? (getMaximum() - getMinimum()) / getInterval() : 0.0;
    const auto pixelsForFullRange = e.mods.isShiftDown() ? 1200.0 : juce::jlimit (220.0, 480.0, steps * 8.0);

    // The default and the marks click in the same way at any speed: the knob rests on one
    // for 8 px of travel, then goes on exactly where the mouse is (no jump, nothing lost).
    // Shift: no detents (fine adjustment).
    const auto zoneNow = e.mods.isShiftDown() ? 0.0 : 8.0 / pixelsForFullRange;
    if (! rawValid || zoneNow != zone)
    {
        zone = zoneNow;
        detents.clear();
        for (const auto v : magneticValues())
            if (const auto p = valueToProportionOfLength (v); p > 1.0e-4 && p < 1.0 - 1.0e-4)
                detents.push_back (p);
        std::sort (detents.begin(), detents.end());
        detents.erase (std::unique (detents.begin(), detents.end(), [] (double a, double b) { return std::abs (a - b) < 1.0e-6; }), detents.end());
        rawPosition = rawFor (valueToProportionOfLength (getValue()), detents, zone);
        rawValid = true;
    }
    rawPosition = juce::jlimit (0.0, 1.0 + zone * (double) detents.size(), rawPosition + delta / pixelsForFullRange);
    setValue (proportionOfLengthToValue (juce::jlimit (0.0, 1.0, knobFor (rawPosition, detents, zone))), juce::sendNotificationSync);
}

double Knob::knobFor (double raw, const std::vector<double>& detents, double zone)
{
    // Each detent takes `zone` of travel in which the knob stays on it.
    auto offset = 0.0;
    for (const auto p : detents)
    {
        if (raw < p + offset)
            return raw - offset;
        if (raw <= p + offset + zone)
            return p;
        offset += zone;
    }
    return raw - offset;
}

double Knob::rawFor (double proportion, const std::vector<double>& detents, double zone)
{
    // Resting on a detent: the middle of its zone (half the zone pulls it off either way).
    auto offset = 0.0;
    for (const auto p : detents)
    {
        if (std::abs (proportion - p) < 1.0e-6)
            return p + offset + zone * 0.5;
        if (proportion < p)
            break;
        offset += zone;
    }
    return proportion + offset;
}

void Knob::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
        e.source.enableUnboundedMouseMovement (false);

    dragging = false;
    juce::Slider::mouseUp (e); // ends the host gesture
    if (onGestureEnd) onGestureEnd();
    repaint();
}

void Knob::nudge (double proportionDelta)
{
    // Accumulate unsnapped so slow changes on stepped parameters (e.g. semitones) still move.
    const auto current = valueToProportionOfLength (getValue());
    const auto wanted = juce::jlimit (0.0, 1.0, current + proportionDelta + residual);
    setValue (proportionOfLengthToValue (wanted), juce::sendNotificationSync);
    residual = wanted - valueToProportionOfLength (getValue());
    if (std::abs (residual) > 0.2)
        residual = 0.0;
}

bool Knob::hasDetents() const
{
    // A real step size with a countable number of steps (not sample positions or ms).
    return getInterval() > 0.0 && (getMaximum() - getMinimum()) / getInterval() <= 200.0;
}

double Knob::notchStep() const
{
    // At most ~13 marks: every step, every 2nd, every octave (12), 25, 50, 100.
    const auto range = getMaximum() - getMinimum();
    for (const auto k : { 1.0, 2.0, 12.0, 25.0, 50.0, 100.0 })
        if (range / (k * getInterval()) <= 13.0)
            return k * getInterval();
    return getInterval();
}

std::vector<double> Knob::magneticValues() const
{
    std::vector<double> values;
    if (isDoubleClickReturnEnabled())
        values.push_back (getDoubleClickReturnValue()); // where the knob started
    // The marks in the ring, when they are coarser than the steps (pitch: -24, -12, +12, +24).
    if (hasDetents() && notchStep() > getInterval() * 1.5)
        for (auto v = getMinimum() + notchStep(); v < getMaximum() - 1.0e-9; v += notchStep())
            values.push_back (v);
    return values;
}

void Knob::stepBy (int steps)
{
    if (onGestureStart) onGestureStart();
    setValue (juce::jlimit (getMinimum(), getMaximum(), getValue() + steps * getInterval()), juce::sendNotificationSync);
    if (onGestureEnd) onGestureEnd();
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;

    if (readoutArea().contains (e.position))
    {
        showTextEntry();
        return;
    }

    if (isDoubleClickReturnEnabled())
    {
        if (onGestureStart) onGestureStart();
        setValue (getDoubleClickReturnValue(), juce::sendNotificationSync);
        if (onGestureEnd) onGestureEnd();
    }
}

void Knob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || wheel.isInertial)
        return;

    const auto amount = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) * (wheel.isReversed ? -1.0f : 1.0f);
    if (hasDetents())
    {
        stepBy (amount > 0 ? 1 : -1); // one notch, one step
        return;
    }
    const auto step = e.mods.isShiftDown() ? 0.004 : 0.025;
    if (onGestureStart) onGestureStart();
    nudge (amount > 0 ? step : -step);
    if (onGestureEnd) onGestureEnd();
}

bool Knob::keyPressed (const juce::KeyPress& key)
{
    if (hasDetents())
    {
        if (key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::rightKey))   { stepBy (1); return true; }
        if (key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::leftKey))  { stepBy (-1); return true; }
    }

    const auto fine = key.getModifiers().isShiftDown();
    const auto step = fine ? 0.004 : 0.02;

    if (key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::rightKey))
    {
        if (onGestureStart) onGestureStart();
        nudge (step);
        if (onGestureEnd) onGestureEnd();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::leftKey))
    {
        if (onGestureStart) onGestureStart();
        nudge (-step);
        if (onGestureEnd) onGestureEnd();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        showTextEntry();
        return true;
    }

    return false;
}

void Knob::showTextEntry()
{
    entry = std::make_unique<juce::TextEditor>();
    entry->setFont (Fonts::get (TextStyle::value));
    entry->setJustification (juce::Justification::centred);
    entry->setText (getTextFromValue (getValue()), false);
    // Inside the knob, or the field's bottom edge is cut off (the readout sits at the bottom).
    auto box = readoutArea().expanded (0.0f, 2.0f);
    if (box.getBottom() > (float) getHeight())
        box.setY ((float) getHeight() - box.getHeight());
    entry->setBounds (box.toNearestInt());
    entry->setSelectAllWhenFocused (true);
    addAndMakeVisible (*entry);
    if (entry->isShowing()) entry->grabKeyboardFocus();

    auto close = [this] (bool apply)
    {
        if (entry == nullptr) return;
        if (apply)
        {
            if (onGestureStart) onGestureStart();
            setValue (getValueFromText (entry->getText()), juce::sendNotificationSync);
            if (onGestureEnd) onGestureEnd();
        }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Knob> (this)] { if (safe != nullptr) { safe->entry.reset(); safe->repaint(); } });
    };

    entry->onReturnKey = [close] { close (true); };
    entry->onEscapeKey = [close] { close (false); };
    entry->onFocusLost = [close] { close (true); };
}

//==============================================================================
PillButton::PillButton (const juce::String& text, bool isToggle) : juce::Button (text)
{
    setClickingTogglesState (isToggle);
    setWantsKeyboardFocus (false); // Space and the shortcuts stay with the editor (a focused button would take Space)
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

//==============================================================================
IconButton::IconButton (const juce::String& name, Icon i, bool toggle, bool record) : juce::Button (name), icon (i), recordStyle (record)
{
    setClickingTogglesState (toggle);
    setWantsKeyboardFocus (false); // keyboard shortcuts stay with the editor
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();
    const auto enabled = isEnabled();
    const auto signal = recordStyle ? Colours::record : Colours::accent;
    constexpr auto pi = juce::MathConstants<float>::pi; // (constexpr: the icons' constexpr angles use it)

    const auto danger = dangerOnHover && highlighted && enabled; // e.g. delete every chop: red under the mouse
    const auto bg = on ? Colours::background.overlaidWith (signal.withAlpha (0.16f))
                       : (danger ? Colours::background.overlaidWith (Colours::record.withAlpha (down ? 0.32f : 0.2f))
                                 : (highlighted && enabled ? Colours::surfaceHover : Colours::surfaceRaised));
    g.setColour (bg);
    g.fillRoundedRectangle (r, Metrics::radiusSmall);
    g.setColour (on ? signal.withAlpha (0.75f) : (danger ? Colours::record : Colours::line));
    g.drawRoundedRectangle (r, Metrics::radiusSmall, danger ? 1.2f : Metrics::hairline);

    if (attention > 0.0f) // drawing the eye (e.g. an update is out): an accent glow that pulses
    {
        g.setColour (Colours::accent.withAlpha (0.18f * attention));
        g.fillRoundedRectangle (r, Metrics::radiusSmall);
        g.setColour (Colours::accent.withAlpha (attention));
        g.drawRoundedRectangle (r, Metrics::radiusSmall, 1.4f);
    }

    auto fg = ! enabled ? Colours::textFaint : (on ? signal : (danger ? Colours::record : (highlighted ? Colours::text : Colours::textDim)));
    if (attention > 0.0f && enabled)
        fg = fg.interpolatedWith (Colours::accent, attention);

    // Symbols are drawn on a 16 x 16 grid, centred in the button.
    const auto box = r.withSizeKeepingCentre (14.0f, 14.0f).translated (0.0f, down ? 0.5f : 0.0f);
    const auto toBox = juce::AffineTransform::scale (box.getWidth() / 16.0f).translated (box.getX(), box.getY())
                           .rotated (turn, box.getCentreX(), box.getCentreY());
    const juce::PathStrokeType line (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (fg);

    switch (on && hasOnIcon ? onIcon : icon)
    {
        case Icon::magnet:
        {
            // Horseshoe magnet: a U with pole tips.
            juce::Path u;
            u.startNewSubPath (4.0f, 2.0f);
            u.lineTo (4.0f, 8.5f);
            u.addCentredArc (8.0f, 8.5f, 4.0f, 4.0f, 0.0f, pi * 1.5f, pi * 0.5f);
            u.lineTo (12.0f, 2.0f);
            g.strokePath (u, juce::PathStrokeType (2.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt), toBox);

            juce::Path gaps; // mark the pole tips
            gaps.addRectangle (2.4f, 4.4f, 3.2f, 0.9f);
            gaps.addRectangle (10.4f, 4.4f, 3.2f, 0.9f);
            g.setColour (bg);
            g.fillPath (gaps, toBox);
            break;
        }

        case Icon::knife:
        {
            // Blade with a straight spine and a curved edge, handle behind, tilted 45 degrees.
            juce::Path knife;
            knife.startNewSubPath (6.2f, 5.0f);
            knife.lineTo (15.6f, 5.0f);
            knife.quadraticTo (14.6f, 10.6f, 6.2f, 11.4f);
            knife.closeSubPath();
            knife.addRoundedRectangle (0.4f, 6.6f, 4.8f, 3.4f, 1.2f);
            g.fillPath (knife, juce::AffineTransform::rotation (-pi * 0.25f, 8.0f, 8.0f).followedBy (toBox));
            break;
        }

        case Icon::undo:
        case Icon::redo:
        {
            // A U-turn arrow: an open arrowhead pointing back (undo) or forward (redo),
            // its shaft bending round into a tail underneath.
            juce::Path arrow;
            arrow.startNewSubPath (3.0f, 5.5f);
            arrow.lineTo (10.0f, 5.5f);
            arrow.addCentredArc (10.0f, 9.25f, 3.75f, 3.75f, 0.0f, 0.0f, pi, false);
            arrow.lineTo (5.5f, 13.0f);
            arrow.startNewSubPath (6.25f, 2.25f);
            arrow.lineTo (3.0f, 5.5f);
            arrow.lineTo (6.25f, 8.75f);
            auto flip = icon == Icon::redo ? juce::AffineTransform::scale (-1.0f, 1.0f, 8.0f, 8.0f) : juce::AffineTransform();
            g.strokePath (arrow, line, flip.followedBy (toBox));
            break;
        }

        case Icon::record:
        {
            // A filled circle (record).
            g.setColour (on ? signal : (enabled ? Colours::record.withAlpha (highlighted ? 1.0f : 0.85f) : Colours::textFaint));
            g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ 8.0f, 8.0f }).transformedBy (toBox));
            break;
        }

        case Icon::capture:
        {
            // Viewfinder corners around a dot: "grab what KERF is playing".
            juce::Path corners;
            const float a = 1.5f, b = 14.5f, l = 4.0f;
            corners.startNewSubPath (a, a + l); corners.lineTo (a, a); corners.lineTo (a + l, a);
            corners.startNewSubPath (b - l, a); corners.lineTo (b, a); corners.lineTo (b, a + l);
            corners.startNewSubPath (b, b - l); corners.lineTo (b, b); corners.lineTo (b - l, b);
            corners.startNewSubPath (a + l, b); corners.lineTo (a, b); corners.lineTo (a, b - l);
            g.strokePath (corners, line, toBox);
            g.setColour (on ? signal : (enabled ? Colours::record.withAlpha (0.85f) : Colours::textFaint));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ 8.0f, 8.0f }).transformedBy (toBox));
            break;
        }

        case Icon::resample:
        {
            // An arrow coming down into a waveform: "the pattern, bounced into a sample".
            juce::Path arrow;
            arrow.startNewSubPath (8.0f, 1.5f);
            arrow.lineTo (8.0f, 7.5f);
            arrow.startNewSubPath (5.2f, 5.0f);
            arrow.lineTo (8.0f, 7.8f);
            arrow.lineTo (10.8f, 5.0f);
            g.strokePath (arrow, line, toBox);
            juce::Path wave;
            const float xs[] = { 1.5f, 3.6f, 5.8f, 8.0f, 10.2f, 12.4f, 14.5f };
            const float ys[] = { 12.5f, 10.6f, 14.2f, 10.0f, 14.2f, 10.6f, 12.5f };
            wave.startNewSubPath (xs[0], ys[0]);
            for (int i = 1; i < 7; ++i)
                wave.lineTo (xs[i], ys[i]);
            g.strokePath (wave, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), toBox);
            break;
        }

        case Icon::previous:
        case Icon::next:
        {
            // A chevron: < previous, > next.
            juce::Path chevron;
            chevron.startNewSubPath (10.0f, 3.5f);
            chevron.lineTo (5.5f, 8.0f);
            chevron.lineTo (10.0f, 12.5f);
            auto flip = icon == Icon::next ? juce::AffineTransform::scale (-1.0f, 1.0f, 8.0f, 8.0f) : juce::AffineTransform();
            g.strokePath (chevron, line, flip.followedBy (toBox));
            break;
        }

        case Icon::load:
        {
            // A folder.
            juce::Path folder;
            folder.startNewSubPath (1.5f, 4.0f);
            folder.lineTo (6.0f, 4.0f);
            folder.lineTo (7.5f, 5.5f);
            folder.lineTo (14.5f, 5.5f);
            folder.lineTo (14.5f, 13.0f);
            folder.lineTo (1.5f, 13.0f);
            folder.closeSubPath();
            g.strokePath (folder, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::square), toBox);
            juce::Path flap;
            flap.startNewSubPath (1.5f, 7.5f);
            flap.lineTo (14.5f, 7.5f);
            g.strokePath (flap, juce::PathStrokeType (1.2f), toBox);
            break;
        }

        case Icon::menu:
        {
            // Three lines: the menu (about, guide, updates).
            juce::Path lines;
            for (const auto y : { 4.0f, 8.0f, 12.0f })
            {
                lines.startNewSubPath (2.5f, y);
                lines.lineTo (13.5f, y);
            }
            g.strokePath (lines, line, toBox);
            break;
        }

        case Icon::play:
        {
            juce::Path triangle;
            triangle.addTriangle (4.5f, 2.5f, 4.5f, 13.5f, 13.5f, 8.0f);
            g.fillPath (triangle, toBox);
            break;
        }

        case Icon::stop:
        {
            juce::Path square;
            square.addRoundedRectangle (3.5f, 3.5f, 9.0f, 9.0f, 1.0f);
            g.fillPath (square, toBox);
            break;
        }

        case Icon::loop:
        {
            // A repeat arrow: almost a full circle, open at the top, its head pointing on round.
            constexpr float radius = 4.9f, a0 = pi * 0.3f, a1 = pi * 1.72f;
            juce::Path ring;
            ring.addCentredArc (8.0f, 8.5f, radius, radius, 0.0f, a0, a1, true);
            g.strokePath (ring, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::butt), toBox);
            const juce::Point<float> tip (8.0f + radius * std::sin (a1), 8.5f - radius * std::cos (a1));
            const juce::Point<float> dir (std::cos (a1), std::sin (a1)), side (-dir.y, dir.x); // along the arc, across it
            juce::Path head;
            head.addTriangle (tip + dir * 3.0f, tip - dir * 0.8f + side * 2.8f, tip - dir * 0.8f - side * 2.8f);
            g.fillPath (head, toBox);
            break;
        }

        case Icon::metronome:
        {
            // A pyramid with its pendulum swung to one side.
            juce::Path body;
            body.startNewSubPath (5.8f, 2.5f);
            body.lineTo (10.2f, 2.5f);
            body.lineTo (13.0f, 13.5f);
            body.lineTo (3.0f, 13.5f);
            body.closeSubPath();
            g.strokePath (body, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered), toBox);
            juce::Path arm;
            arm.startNewSubPath (8.0f, 11.5f);
            arm.lineTo (12.6f, 4.0f);
            g.strokePath (arm, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), toBox);
            g.fillEllipse (juce::Rectangle<float> (2.6f, 2.6f).withCentre ({ 10.9f, 6.8f }).transformedBy (toBox));
            break;
        }

        case Icon::dice:
        {
            // A die showing five.
            juce::Path face;
            face.addRoundedRectangle (2.5f, 2.5f, 11.0f, 11.0f, 2.2f);
            g.strokePath (face, juce::PathStrokeType (1.4f), toBox);
            for (const auto [x, y] : { std::pair { 5.6f, 5.6f }, { 10.4f, 5.6f }, { 8.0f, 8.0f }, { 5.6f, 10.4f }, { 10.4f, 10.4f } })
                g.fillEllipse (juce::Rectangle<float> (1.9f, 1.9f).withCentre ({ x, y }).transformedBy (toBox));
            break;
        }
        case Icon::trash:
        {
            // A bin: lid with a handle, a tapered body with two ribs.
            juce::Path lid;
            lid.startNewSubPath (2.5f, 4.0f);
            lid.lineTo (13.5f, 4.0f);
            lid.startNewSubPath (6.2f, 4.0f);
            lid.lineTo (6.2f, 2.2f);
            lid.lineTo (9.8f, 2.2f);
            lid.lineTo (9.8f, 4.0f);
            g.strokePath (lid, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt), toBox);
            juce::Path body;
            body.startNewSubPath (4.0f, 5.8f);
            body.lineTo (4.9f, 14.0f);
            body.lineTo (11.1f, 14.0f);
            body.lineTo (12.0f, 5.8f);
            g.strokePath (body, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt), toBox);
            juce::Path ribs;
            ribs.startNewSubPath (6.9f, 7.5f); ribs.lineTo (7.1f, 12.2f);
            ribs.startNewSubPath (9.1f, 7.5f); ribs.lineTo (8.9f, 12.2f);
            g.strokePath (ribs, juce::PathStrokeType (1.1f), toBox);
            break;
        }

        case Icon::pause:
        {
            juce::Path bars;
            bars.addRoundedRectangle (4.0f, 3.0f, 3.0f, 10.0f, 0.8f);
            bars.addRoundedRectangle (9.0f, 3.0f, 3.0f, 10.0f, 0.8f);
            g.fillPath (bars, toBox);
            break;
        }

        case Icon::plus:
        case Icon::minus:
        {
            juce::Path p;
            p.startNewSubPath (3.0f, 8.0f);
            p.lineTo (13.0f, 8.0f);
            if (icon == Icon::plus)
            {
                p.startNewSubPath (8.0f, 3.0f);
                p.lineTo (8.0f, 13.0f);
            }
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded), toBox);
            break;
        }
    }
}

int PillButton::getIdealWidth() const
{
    return (int) textWidth (getButtonText().toUpperCase(), TextStyle::label) + 20;
}

void PillButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();
    const auto enabled = isEnabled();
    const auto signal = recordStyle ? Colours::record : Colours::accent;

    juce::Colour bg = Colours::surfaceRaised, border = Colours::line, fg = Colours::textDim;

    if (primary)
    {
        // Filled accent with the background colour as text: unmistakably "the" action.
        const auto fill = ! enabled ? Colours::surfaceRaised
                        : (down ? Colours::accent.darker (0.15f) : (highlighted ? Colours::accent.brighter (0.12f) : Colours::accent));
        g.setColour (fill);
        g.fillRoundedRectangle (r, Metrics::radiusSmall);
        if (! enabled)
        {
            g.setColour (Colours::hairline);
            g.drawRoundedRectangle (r, Metrics::radiusSmall, Metrics::hairline);
        }
        drawText (g, getButtonText().toUpperCase(), r, TextStyle::label, enabled ? Colours::background : Colours::textFaint,
                  juce::Justification::centred);
        return;
    }


    if (on && accentWhenOn)
    {
        bg = signal.withAlpha (0.14f);
        border = signal.withAlpha (0.7f);
        fg = signal;
    }
    else if (on)
    {
        bg = Colours::surfaceHover;
        fg = Colours::text;
    }

    if (highlighted && enabled) { bg = bg.brighter (0.08f); fg = on ? fg : Colours::text; }
    if (down && enabled)        bg = bg.darker (0.2f);
    if (! enabled)              { fg = Colours::textFaint; border = Colours::hairline; }

    g.setColour (bg);
    g.fillRoundedRectangle (r, Metrics::radiusSmall);
    g.setColour (border);
    g.drawRoundedRectangle (r, Metrics::radiusSmall, Metrics::hairline);
    drawText (g, getButtonText().toUpperCase(), r, TextStyle::label, fg, juce::Justification::centred);
}

//==============================================================================
Segmented::Segmented (juce::StringArray opts, int sel) : options (std::move (opts)), selected (sel)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void Segmented::setOptions (juce::StringArray newOptions)
{
    options = std::move (newOptions);
    selected = juce::jlimit (0, std::max (0, options.size() - 1), selected);
    repaint();
}

void Segmented::setSelected (int index, juce::NotificationType n)
{
    index = juce::jlimit (0, std::max (0, options.size() - 1), index);
    if (index == selected)
        return;
    selected = index;
    repaint();
    if (n != juce::dontSendNotification && onChange)
        onChange (selected);
}

juce::Rectangle<float> Segmented::cellBounds (int i) const
{
    // One row (default), one column (vertical) or a grid of `columns`.
    const auto r = getLocalBounds().toFloat();
    const auto n = std::max (1, options.size());
    const auto cols = vertical ? 1 : (columns > 0 ? std::min (columns, n) : n);
    const auto rowsCount = (n + cols - 1) / cols;
    const auto w = r.getWidth() / (float) cols, h = r.getHeight() / (float) rowsCount;
    return { r.getX() + w * (float) (i % cols), r.getY() + h * (float) (i / cols), w, h };
}

int Segmented::indexAt (juce::Point<int> p) const
{
    if (options.isEmpty() || ! getLocalBounds().contains (p))
        return -1;
    for (int i = 0; i < options.size(); ++i)
        if (cellBounds (i).contains (p.toFloat()))
            return i;
    return -1;
}

void Segmented::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (Colours::surfaceRaised);
    g.fillRoundedRectangle (r.reduced (0.5f), Metrics::radiusSmall);
    g.setColour (Colours::line);
    g.drawRoundedRectangle (r.reduced (0.5f), Metrics::radiusSmall, Metrics::hairline);

    for (int i = 0; i < options.size(); ++i)
    {
        const auto cell = cellBounds (i);
        const auto w = cell.getWidth(), h = cell.getHeight();

        // Separators: left edge inside a row, top edge between rows.
        if (cell.getX() > r.getX() + 0.5f)
            drawHairline (g, { cell.getX(), cell.getY() + 4.0f, 1.0f, h - 8.0f });
        if (cell.getY() > r.getY() + 0.5f)
            drawHairline (g, { cell.getX() + 4.0f, cell.getY(), w - 8.0f, 1.0f });

        const bool isSel = i == selected;
        if (isSel)
        {
            g.setColour (Colours::surfaceHover);
            g.fillRect (cell.reduced (1.0f));
            g.setColour (Colours::accent);
            if (vertical) // a short bar on the left edge
                g.fillRect (cell.reduced (0.0f, h * 0.28f).withWidth (1.5f).withX (cell.getX() + 2.0f));
            else
                g.fillRect (cell.reduced (w * 0.3f, 0.0f).withHeight (1.5f).withBottomY (cell.getBottom() - 2.0f));
        }

        drawText (g, options[i], cell, TextStyle::label,
                  isSel ? Colours::text : (i == hovered ? Colours::text.withAlpha (0.8f) : Colours::textDim),
                  juce::Justification::centred);
    }
}

void Segmented::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return; // a right-click: nothing
    if (const auto i = indexAt (e.getPosition()); i >= 0)
    {
        if (i == selected && onChange)
            onChange (selected); // re-click re-applies (useful for chop modes)
        else
            setSelected (i);
    }
}

void Segmented::mouseMove (const juce::MouseEvent& e)
{
    const auto i = indexAt (e.getPosition());
    if (i != hovered) { hovered = i; repaint(); }
}

void Segmented::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

//==============================================================================
ValueField::ValueField (double minValue, double maxValue, double s, int d)
    : minV (minValue), maxV (maxValue), step (s), decimals (d)
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void ValueField::setValue (double v, juce::NotificationType n)
{
    v = juce::jlimit (minV, maxV, v);
    if (step > 0.0)
        v = std::round (v / step) * step;
    if (juce::approximatelyEqual (v, value) && hasValue)
        return;
    value = v;
    hasValue = true;
    repaint();
    if (n != juce::dontSendNotification && onChange)
        onChange (value);
}

void ValueField::paint (juce::Graphics& g)
{
    if (entry != nullptr)
        return;

    const auto text = ! hasValue ? placeholder : (formatter ? formatter (value) : juce::String (value, decimals) + suffix);
    const auto colour = ! isEnabled() ? Colours::textFaint : (dragging ? Colours::accent : (dimmed ? Colours::textDim : Colours::text));
    drawText (g, text, getLocalBounds().toFloat(), large ? TextStyle::valueLarge : TextStyle::value, colour, justification);
}

void ValueField::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onPopupMenu)
    {
        onPopupMenu();
        return;
    }
    dragStartY = e.position.y;
    dragStartValue = hasValue ? value : (minV + maxV) * 0.5;
}

void ValueField::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || wheel.isInertial || entry != nullptr)
        return;
    const auto amount = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) * (wheel.isReversed ? -1.0f : 1.0f);
    if (amount == 0.0f)
        return;

    // One step per notch: the drag step (BPM: whole BPM, 92.3 -> 93 -> 94), Shift the fine one.
    const auto s = ! e.mods.isShiftDown() && dragStep > step ? dragStep : (step > 0.0 ? step : 1.0);
    const auto base = hasValue ? value : (minV + maxV) * 0.5;
    const auto next = amount > 0.0f ? std::floor (base / s + 1.0e-9) * s + s
                                    : std::ceil (base / s - 1.0e-9) * s - s;
    setValue (next, juce::sendNotification);
}

void ValueField::mouseDrag (const juce::MouseEvent& e)
{
    if (! isEnabled() || e.mods.isPopupMenu())
        return;

    if (! dragging && std::abs (e.position.y - dragStartY) < 3.0f)
        return;

    dragging = true;
    const auto fine = e.mods.isShiftDown();
    const auto pixelsPerStep = fine ? 12.0 : 3.0;
    const auto steps = std::round ((dragStartY - e.position.y) / pixelsPerStep);
    if (! fine && dragStep > step)
    {
        // Whole drag steps (BPM: 92.3 -> 93 -> 94 ...); Shift keeps the fine step.
        if (steps != 0.0)
            setValue ((steps > 0.0 ? std::floor (dragStartValue / dragStep) : std::ceil (dragStartValue / dragStep)) * dragStep + steps * dragStep,
                      juce::sendNotification);
        return;
    }
    setValue (dragStartValue + steps * (step > 0.0 ? step : 1.0), juce::sendNotification);
}

void ValueField::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void ValueField::mouseDoubleClick (const juce::MouseEvent&)
{
    if (! isEnabled())
        return;

    // The box exactly over the field (wider, its edges were cut off), the number where the
    // field shows it (it jumped to the left), centred in the height.
    entry = std::make_unique<juce::TextEditor>();
    const auto font = Fonts::get (large ? TextStyle::valueLarge : TextStyle::value);
    entry->setFont (font);
    entry->setJustification (justification.getOnlyHorizontalFlags() | juce::Justification::verticallyCentred);
    entry->setBorder ({});
    entry->setIndents (3, juce::jmax (0, juce::roundToInt ((getHeight() - font.getHeight()) * 0.5f)));
    entry->setText (hasValue ? juce::String (value, decimals) : juce::String(), false);
    entry->setBounds (getLocalBounds());
    entry->setSelectAllWhenFocused (true);
    entry->setInputRestrictions (8, "0123456789.");
    addAndMakeVisible (*entry);
    if (entry->isShowing()) entry->grabKeyboardFocus();

    auto close = [this] (bool apply)
    {
        if (entry == nullptr) return;
        if (apply && entry->getText().isNotEmpty())
            setValue (entry->getText().getDoubleValue(), juce::sendNotification);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ValueField> (this)] { if (safe != nullptr) { safe->entry.reset(); safe->repaint(); } });
    };

    entry->onReturnKey = [close] { close (true); };
    entry->onEscapeKey = [close] { close (false); };
    entry->onFocusLost = [close] { close (true); };
}

//==============================================================================
CentreSlider::CentreSlider (double r, double s, int d, juce::String sfx)
    : range (r), step (s), decimals (d), suffix (std::move (sfx))
{
    setRepaintsOnMouseActivity (true);
}

void CentreSlider::setValue (double v, juce::NotificationType notification)
{
    v = juce::jlimit (-range, range, step > 0.0 ? std::round (v / step) * step : v);
    if (v == 0.0)
        v = 0.0; // (not -0)
    if (v == value)
        return;
    value = v;
    repaint();
    if (notification != juce::dontSendNotification && onChange)
        onChange (value);
}

juce::Rectangle<float> CentreSlider::trackArea() const
{
    return getLocalBounds().toFloat().withTrimmedRight (62.0f).reduced (6.0f, 0.0f);
}

void CentreSlider::paint (juce::Graphics& g)
{
    const auto track = trackArea();
    const auto y = track.getCentreY();
    const auto centre = track.getCentreX();
    const auto x = centre + (float) (value / range) * track.getWidth() * 0.5f;
    const auto on = isEnabled();

    g.setColour (Colours::line.withAlpha (on ? 1.0f : 0.5f));
    g.fillRoundedRectangle (track.withHeight (2.0f).withCentre ({ centre, y }), 1.0f);
    g.fillRect (juce::Rectangle<float> (1.0f, 8.0f).withCentre ({ centre, y }));       // the zero
    if (on && value != 0.0)
    {
        g.setColour (Colours::accent);
        g.fillRect (juce::Rectangle<float> (std::min (x, centre), y - 1.0f, std::abs (x - centre), 2.0f));
    }
    const auto hot = dragging || isMouseOver (true);
    g.setColour (! on ? Colours::textFaint : (dragging ? Colours::accent : (hot ? Colours::text : Colours::textDim)));
    g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, 14.0f).withCentre ({ x, y }), 1.5f);

    const auto text = (value > 0.0 ? "+" : "") + juce::String (value, decimals) + suffix;
    drawText (g, text, getLocalBounds().toFloat().removeFromRight (60.0f), TextStyle::value,
              ! on ? Colours::textFaint : (value != 0.0 ? Colours::text : Colours::textDim), juce::Justification::centredRight);
}

void CentreSlider::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled() || e.mods.isPopupMenu())
        return;
    dragging = true;
    lastX = e.position.x;
    dragValue = value;
    repaint();
}

void CentreSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    const auto perPixel = 2.0 * range / std::max (20.0f, trackArea().getWidth()) * (e.mods.isShiftDown() ? 0.1 : 1.0);
    dragValue = juce::jlimit (-range, range, dragValue + (e.position.x - lastX) * perPixel);
    lastX = e.position.x;
    setValue (dragValue, juce::sendNotification);
}

void CentreSlider::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    repaint();
    if (onDragEnd)
        onDragEnd();
}

void CentreSlider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (isEnabled())
        setValue (0.0, juce::sendNotification);
}

void CentreSlider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || wheel.isInertial)
        return;
    const auto amount = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) * (wheel.isReversed ? -1.0f : 1.0f);
    if (amount == 0.0f)
        return;
    const auto nudge = e.mods.isShiftDown() ? step : step * 10.0;
    setValue (value + (amount > 0.0f ? nudge : -nudge), juce::sendNotification);
}

//==============================================================================
void drawHairline (juce::Graphics& g, juce::Rectangle<float> line, juce::Colour c)
{
    g.setColour (c);
    g.fillRect (line);
}

void drawSectionHeader (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title, const juce::String& tag, bool tagIsAccent)
{
    drawText (g, title, area, TextStyle::sectionLabel, Colours::text.withAlpha (0.85f));

    if (tag.isNotEmpty())
        drawText (g, tag, area, TextStyle::micro, tagIsAccent ? Colours::accent : Colours::textFaint, juce::Justification::centredRight);
}

} // namespace kerf::ui
