#include "TimeView.h"
#include "ScreenStyle.h"

namespace kerf::ui
{

namespace
{
    using screen::signal;
    using screen::hot;
    using screen::hudText;
    using screen::glowStroke;

    constexpr float pi = juce::MathConstants<float>::pi;

    int activeSlot (KerfAudioProcessor& p)
    {
        return (int) p.getParameters().getRawParameterValue (Params::timeSlot)->load() - 1; // -1 = off
    }

    /** Grid steps per cycle: sixteenths (Shift while dragging: free). */
    int snapSteps (KerfAudioProcessor& p)
    {
        return juce::jmax (1, (int) std::round (16.0 * p.getTimeCycleBeats() / 4.0));
    }

    /** How fast the tape runs along a TIME segment: 1 normal, below 1 slower, 0 stopped,
        negative backwards. */
    juce::Colour speedColour (float speed)
    {
        if (speed <= 0.02f)                 return hot (0.55f);                                   // stopped / backwards
        if (speed < 0.98f)                  return signal().interpolatedWith (nebulaGlow(), 0.35f + 0.65f * (1.0f - speed)); // slower
        if (speed > 1.02f)                  return signal().brighter (0.35f);                     // catching up
        return signal();
    }

    juce::String speedText (float speed)
    {
        if (std::abs (speed) < 0.03f) return "STOP";
        return (speed < 0.0f ? "REV " : "") + juce::String (std::abs (speed), 2) + "x";
    }
}

int timeEditSlot (KerfAudioProcessor& p)
{
    if (const auto active = activeSlot (p); active >= 0)
        return active;
    return juce::jlimit (0, TimeFactory::numSlots - 1, (int) p.getSettings().getProperty ("timeEditSlot", TimeFactory::customSlot));
}

void setTimeEditSlot (KerfAudioProcessor& p, int slot)
{
    p.getSettings().setProperty ("timeEditSlot", juce::jlimit (0, TimeFactory::numSlots - 1, slot), nullptr);
}

//==============================================================================
TimeView::TimeView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    speedHistory.fill (1.0f);
    addAndMakeVisible (reset);
    reset.setTooltip ("Back to the slot's original shape");
    reset.onClick = [this] { processor.resetTimeSlot (shownSlot); };
    clock.add (this);
}

TimeView::~TimeView()
{
    clock.remove (this);
}

void TimeView::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (26.0f);
    r.removeFromTop (2.0f);

    auto right = r.removeFromRight (318.0f);
    r.removeFromRight (10.0f);
    timeScreen = r.removeFromTop (r.getHeight() * 0.66f);
    r.removeFromTop (8.0f);
    volumeScreen = r;

    tapeScreen = right.removeFromTop (right.getHeight() * 0.6f);
    right.removeFromTop (8.0f);
    speedScreen = right;
    if (shownSlot >= 0)
        placeReset();
}

juce::String TimeView::slotTitle() const
{
    return "SLOT " + juce::String (shownSlot + 1).paddedLeft ('0', 2) + "  " + TimeFactory::name (shownSlot)
           + (processor.isTimeSlotEdited (shownSlot) ? "  (EDITED)" : "");
}

void TimeView::placeReset()
{
    // Small, on the header line right after the slot's name; usable once the slot is edited.
    placedFor = slotTitle();
    const auto x = header.getX() + 12.0f + 52.0f + textWidth (placedFor, TextStyle::micro) + 10.0f;
    reset.setBounds (juce::Rectangle<float> (x, header.getCentreY() - 9.0f, 46.0f, 18.0f).toNearestInt());
    reset.setEnabled (processor.isTimeSlotEdited (shownSlot));
}

juce::Rectangle<float> TimeView::plotArea (int lane) const
{
    const auto s = lane == timeLane ? timeScreen : volumeScreen;
    return s.reduced (44.0f, 0.0f).withTrimmedTop (26.0f).withTrimmedBottom (lane == timeLane ? 22.0f : 18.0f);
}

juce::Point<float> TimeView::toScreen (int lane, float x, float y) const
{
    const auto a = plotArea (lane);
    // TIME: live at the top, further behind going down. VOLUME: full at the top.
    return { a.getX() + x * a.getWidth(), lane == timeLane ? a.getY() + y / lagRange * a.getHeight() : a.getBottom() - y * a.getHeight() };
}

juce::Point<float> TimeView::fromScreen (int lane, juce::Point<float> p, bool snap) const
{
    const auto a = plotArea (lane);
    auto x = juce::jlimit (0.0f, 1.0f, (p.x - a.getX()) / a.getWidth());
    auto y = juce::jlimit (0.0f, 1.0f, lane == timeLane ? (p.y - a.getY()) / a.getHeight() * lagRange : (a.getBottom() - p.y) / a.getHeight());
    if (snap)
    {
        if (const auto steps = snapSteps (processor); steps > 0)
        {
            x = std::round (x * (float) steps) / (float) steps;
            if (lane == timeLane) y = std::round (y * (float) steps) / (float) steps; // the lag snaps to the same grid
        }
        if (lane == volumeLane) y = std::round (y * 20.0f) / 20.0f;                   // 5 % steps
    }
    return { x, y };
}

int TimeView::laneAt (juce::Point<float> p) const
{
    if (plotArea (timeLane).expanded (6.0f).contains (p))   return timeLane;
    if (plotArea (volumeLane).expanded (6.0f).contains (p)) return volumeLane;
    return -1;
}

TimeView::Hit TimeView::hitTest (juce::Point<float> p) const
{
    Hit best;
    float bestDistance = 9.0f;
    for (const auto lane : { timeLane, volumeLane })
    {
        const auto& c = lane == timeLane ? shown.time : shown.volume;
        for (int i = 0; i < c.numPoints; ++i)
            if (const auto d = p.getDistanceFrom (toScreen (lane, c.x[(size_t) i], c.y[(size_t) i])); d < bestDistance)
            {
                bestDistance = d;
                best = { lane, i };
            }
    }
    return best;
}

//==============================================================================
void TimeView::animationTick (double dt)
{
    time += dt;
    if (intro < 1.0f)
        intro = std::min (1.0f, intro + (float) dt / 0.8f);

    const auto previousSlot = shownSlot;
    const auto previous = shown;
    shownSlot = timeEditSlot (processor);
    shown = processor.getTimeSlot (shownSlot);
    const auto edited = shownSlot != previousSlot || ! (shown.time == previous.time) || ! (shown.volume == previous.volume);
    if (slotTitle() != placedFor)
        placeReset(); // another slot, or it was edited / reset

    // The TIME screen shows as far back as the curve goes, with room below to drag further
    // (it grows while you drag down), in quarters of the cycle.
    {
        float deepest = 0.0f;
        for (int i = 0; i < shown.time.numPoints; ++i)
            deepest = std::max (deepest, shown.time.y[(size_t) i]);
        lagTarget = juce::jlimit (0.125f, 1.0f, std::ceil (deepest * 8.0f - 0.001f) / 8.0f + 0.125f);
        lagRange += (lagTarget - lagRange) * std::min (1.0f, (float) dt * 8.0f);
    }

    const auto& shaper = processor.getTimeShaper();
    const auto live = shaper.isEngaged() && activeSlot (processor) == shownSlot;
    engaged += ((live ? 1.0f : 0.0f) - engaged) * std::min (1.0f, (float) dt * 6.0f);
    beam = shaper.getDisplayPosition();
    const auto playing = shaper.isClockRunning() && processor.isProcessing(); // a host that stops processing has stopped too
    running += ((playing ? 1.0f : 0.0f) - running) * std::min (1.0f, (float) dt * 8.0f); // the beam's trail

    // The tape: its speed glides (reels have weight), and the trace scrolls on.
    const auto target = live ? shaper.getDisplaySpeed() : 1.0f;
    speed += (target - speed) * std::min (1.0f, (float) dt * 10.0f);
    // Each reel keeps its own angle (wrapping one from the other made the right one jump);
    // the smaller take-up pack turns faster.
    // The tape only runs while something plays (the song, the pattern, a chop): it spins
    // up and runs down with weight; the SPEED trace scrolls only then too.
    const auto tapeOn = playing || processor.isSounding();
    tapeRun += ((tapeOn ? 1.0f : 0.0f) - tapeRun) * std::min (1.0f, (float) dt * 4.0f);
    if (tapeRun < 0.001f && ! tapeOn) tapeRun = 0.0f;
    reelAngle = std::fmod (reelAngle + (float) dt * speed * 4.5f * tapeRun, 2.0f * pi);
    reelAngleRight = std::fmod (reelAngleRight + (float) dt * speed * 4.5f * (0.86f / 0.58f) * tapeRun, 2.0f * pi);
    if (tapeRun > 0.0f)
    {
        speedHistory[(size_t) speedWrite] = speed;
        speedWrite = (speedWrite + 1) % (int) speedHistory.size();
    }

    // At rest (TIME not playing, silence, nothing edited, the axis settled): no repaint.
    const auto active = engaged > 0.01f || playing || running > 0.01f || tapeRun > 0.0f || std::abs (target - speed) > 0.001f || intro < 1.0f || edited
                     || std::abs (lagRange - lagTarget) > 0.001f || processor.isSounding();
    if (idle.update (dt, active, processor.getParameterChangeCount()) && isShowing())
        repaint();
}

void TimeView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat(), Backdrop::deep);

    // --- Header ---------------------------------------------------------------------------
    {
        auto h = header.reduced (12.0f, 0.0f);
        drawText (g, "TIME", h.removeFromLeft (52.0f), TextStyle::sectionLabel, Colours::text);
        const auto active = activeSlot (processor) == shownSlot;
        drawText (g, slotTitle(), h.removeFromLeft (220.0f), TextStyle::micro, active ? Colours::accent : Colours::textDim);

        static const char* cycles[] = { "1/2 BAR", "1 BAR", "2 BARS" };
        const auto mix = processor.getParameters().getRawParameterValue (Params::timeMix)->load();
        const auto cycle = cycles[juce::jlimit (0, 2, (int) processor.getParameters().getRawParameterValue (Params::timeLength)->load())];
        const auto tabOff = processor.getParameters().getRawParameterValue (Params::timeOff)->load() > 0.5f;
        const auto readout = (tabOff ? juce::String ("TAB OFF (DOUBLE-CLICK IT)   ") : (active ? juce::String ("ON   ") : juce::String ("OFF   "))) + "SPEED " + speedText (speed)
                           + "   BEHIND " + juce::String (active ? processor.getTimeShaper().getDisplayLagBeats() : 0.0f, 2) + " BEATS"
                           + "   CYCLE " + cycle + "   MIX " + juce::String (juce::roundToInt (mix * 100.0f)) + "%";
        drawText (g, readout, h, TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }

    paintCurveScreen (g, timeLane);
    paintCurveScreen (g, volumeLane);
    paintTape (g, tapeScreen);
    paintSpeed (g, speedScreen);

    // Screens switch on one after another when the page opens.
    const juce::Rectangle<float> screens[] = { timeScreen, volumeScreen, tapeScreen, speedScreen };
    for (int i = 0; i < 4; ++i)
        screen::powerOn (g, screens[i], juce::jlimit (0.0f, 1.0f, intro * 1.5f - 0.12f * (float) i));
}

void TimeView::paintCurveScreen (juce::Graphics& g, int lane)
{
    const auto s = lane == timeLane ? timeScreen : volumeScreen;
    screen::paintScreen (g, s, lane == timeLane ? "TIME" : "VOLUME", 0.6f, this);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (s.reduced (1.0f).toNearestInt());

    const auto a = plotArea (lane);
    const auto cycleBeats = processor.getTimeCycleBeats();
    const auto sixteenths = juce::jmax (1, (int) std::round (cycleBeats * 4.0));

    // Grid: sixteenths faint, beats brighter, bars brightest; numbered beats underneath.
    for (int k = 0; k <= sixteenths; ++k)
    {
        const auto x = a.getX() + a.getWidth() * (float) k / (float) sixteenths;
        const auto bar = k % 16 == 0, beat = k % 4 == 0;
        g.setColour (screen::hud().withAlpha (bar ? 0.16f : (beat ? 0.09f : 0.04f)));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, a.getY(), 1.0f, a.getHeight()));
        if (beat && k < sixteenths && lane == timeLane)
            hudText (g, juce::String (k / 4 + 1), { x + 3.0f, a.getBottom() + 4.0f, 20.0f, 12.0f }, bar ? 0.55f : 0.3f);
    }
    // Rows: TIME in beats behind (as far as the screen reaches), VOLUME in percent.
    const auto beatsShown = lane == timeLane ? lagRange * (float) cycleBeats : 4.0f;
    const auto rowStep = lane == timeLane ? (beatsShown <= 1.01f ? 0.25f : (beatsShown <= 2.01f ? 0.5f : 1.0f)) : 1.0f;
    for (float k = 0.0f; k <= beatsShown + 0.01f; k += rowStep)
    {
        const auto y = lane == timeLane ? toScreen (lane, 0.0f, k / (float) cycleBeats).y : a.getY() + a.getHeight() * k / 4.0f;
        if (y > a.getBottom() + 0.5f)
            break;
        g.setColour (screen::hud().withAlpha (k == 0.0f ? 0.14f : 0.05f));
        g.fillRect (juce::Rectangle<float> (a.getX(), y - 0.5f, a.getWidth(), 1.0f));
        const auto label = lane == timeLane ? (k == 0.0f ? juce::String ("LIVE") : "-" + juce::String (k, rowStep < 0.5f ? 2 : (rowStep < 1.0f ? 1 : 0)))
                                            : juce::String (juce::roundToInt (100.0f - 25.0f * k));
        hudText (g, label, { a.getX() - 42.0f, y - 6.0f, 36.0f, 12.0f }, k == 0.0f ? 0.55f : 0.3f, juce::Justification::centredRight);
    }

    const auto& c = lane == timeLane ? shown.time : shown.volume;
    if (c.numPoints < 2)
        return;

    // The curve, drawn in as the page opens.
    const auto reveal = Motion::easeOut (juce::jlimit (0.0f, 1.0f, intro * 1.3f - 0.15f));
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (a.withWidth (a.getWidth() * reveal).expanded (8.0f, 8.0f).toNearestInt());

        // Fill between the reference line (live / silent) and the curve.
        juce::Path fill;
        const auto base = lane == timeLane ? a.getY() : a.getBottom();
        fill.startNewSubPath (a.getX(), base);
        for (int i = 0; i < c.numPoints; ++i)
            fill.lineTo (toScreen (lane, c.x[(size_t) i], c.y[(size_t) i]));
        fill.lineTo (a.getRight(), base);
        fill.closeSubPath();
        const auto tint = lane == timeLane ? signal() : nebulaGlow().interpolatedWith (signal(), 0.3f);
        g.setGradientFill (juce::ColourGradient (tint.withAlpha (lane == timeLane ? 0.05f : 0.22f), 0.0f, a.getY(),
                                                 tint.withAlpha (lane == timeLane ? 0.22f : 0.04f), 0.0f, a.getBottom(), false));
        g.fillPath (fill);

        // Segments: TIME coloured by how fast the tape runs there; jumps dashed.
        for (int i = 0; i + 1 < c.numPoints; ++i)
        {
            const auto p0 = toScreen (lane, c.x[(size_t) i], c.y[(size_t) i]);
            const auto p1 = toScreen (lane, c.x[(size_t) i + 1], c.y[(size_t) i + 1]);
            juce::Path seg;
            seg.startNewSubPath (p0);
            seg.lineTo (p1);
            const auto dx = c.x[(size_t) i + 1] - c.x[(size_t) i];
            if (dx < 1.0e-4f)
            {
                juce::Path dashed;
                const float dashes[] = { 2.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, seg, dashes, 2);
                g.setColour (screen::hud().withAlpha (0.35f));
                g.fillPath (dashed);
                continue;
            }
            const auto colour = lane == timeLane ? speedColour (1.0f - (c.y[(size_t) i + 1] - c.y[(size_t) i]) / dx)
                                                 : nebulaGlow().interpolatedWith (signal(), 0.3f);
            glowStroke (g, seg, colour, 1.8f, 0.7f);
        }
    }

    // Points: handles (the first and last stay at the ends of the cycle).
    for (int i = 0; i < c.numPoints; ++i)
    {
        const auto p = toScreen (lane, c.x[(size_t) i], c.y[(size_t) i]);
        const auto hot = (hover.lane == lane && hover.index == i) || (drag.lane == lane && drag.index == i);
        const auto r = hot ? 5.0f : (c.numPoints > 24 ? 2.2f : 3.2f); // dense curves (GATE 16): smaller handles
        g.setColour (screen::bg());
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p));
        g.setColour (hot ? screen::hot (0.4f) : signal().withAlpha (0.9f));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p), hot ? 1.6f : 1.2f);
    }

    // The beat: a beam across the screen (always: it runs with the song, stands where the
    // song stands when stopped) and, with this slot on, a dot riding the curve.
    {
        const auto strength = 0.45f + 0.55f * engaged;
        const auto x = a.getX() + beam * a.getWidth();
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.0f), x - 26.0f, 0.0f, signal().withAlpha (0.16f * strength * running), x, 0.0f, false));
        g.fillRect (juce::Rectangle<float> (x - 26.0f, a.getY(), 26.0f, a.getHeight()));
        g.setColour (hot (0.3f).withAlpha (0.8f * strength));
        g.fillRect (juce::Rectangle<float> (x - 0.75f, a.getY(), 1.5f, a.getHeight()));
    }
    if (engaged > 0.01f)
    {
        const auto dot = toScreen (lane, beam, c.valueAt (beam));
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.5f * engaged), dot.x, dot.y, signal().withAlpha (0.0f), dot.x + 14.0f, dot.y, true));
        g.fillEllipse (juce::Rectangle<float> (28.0f, 28.0f).withCentre (dot));
        g.setColour (hot (0.6f).withAlpha (engaged));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (dot));
        if (lane == timeLane)
            hudText (g, speedText (speed), { dot.x + 9.0f, dot.y - 15.0f, 70.0f, 12.0f }, 0.8f * engaged);
    }

    screen::scanlines (g, s, 0.06f);
}

void TimeView::paintTape (juce::Graphics& g, juce::Rectangle<float> s)
{
    screen::paintScreen (g, s, "TAPE", 0.6f, this);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (s.reduced (1.0f).toNearestInt());

    const auto area = s.reduced (16.0f).withTrimmedTop (16.0f).withTrimmedBottom (26.0f);
    const auto r = juce::jmin (area.getHeight() * 0.42f, area.getWidth() * 0.21f);
    const juce::Point<float> left (area.getCentreX() - r * 1.25f, area.getY() + r + 4.0f), right (area.getCentreX() + r * 1.25f, area.getY() + r + 4.0f);
    const juce::Point<float> head (area.getCentreX(), area.getBottom() - 4.0f);

    // Tape: from the supply pack round the head to the take-up pack.
    const auto supply = r * 0.86f, takeUp = r * 0.58f;
    {
        juce::Path tape;
        tape.startNewSubPath (left.x - supply * 0.2f, left.y + supply);
        tape.quadraticTo (left.x + r * 0.4f, head.y + 2.0f, head.x - 8.0f, head.y);
        tape.lineTo (head.x + 8.0f, head.y);
        tape.quadraticTo (right.x - r * 0.4f, head.y + 2.0f, right.x + takeUp * 0.2f, right.y + takeUp);
        g.setColour (screen::hud().withAlpha (0.45f));
        g.strokePath (tape, juce::PathStrokeType (1.4f));
    }
    // The head glows while TIME plays.
    {
        const auto box = juce::Rectangle<float> (26.0f, 9.0f).withCentre (head.translated (0.0f, 6.0f));
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.45f * engaged), box.getCentreX(), box.getCentreY(),
                                                 signal().withAlpha (0.0f), box.getCentreX() + 30.0f, box.getCentreY(), true));
        g.fillEllipse (box.expanded (24.0f, 14.0f));
        g.setColour (screen::bg());
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (signal().interpolatedWith (screen::hud(), 1.0f - engaged).withAlpha (0.8f));
        g.drawRoundedRectangle (box, 2.0f, 1.2f);
    }

    // The reels: a pack of tape round a hub with three windows, turning with the speed.
    auto reel = [&] (juce::Point<float> centre, float pack, float angle)
    {
        g.setColour (screen::hud().withAlpha (0.10f));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.05f + 0.12f * engaged), centre.x, centre.y,
                                                 signal().withAlpha (0.18f + 0.2f * engaged), centre.x + pack, centre.y, true));
        g.fillEllipse (juce::Rectangle<float> (pack * 2.0f, pack * 2.0f).withCentre (centre));
        g.setColour (signal().withAlpha (0.35f + 0.4f * engaged));
        g.drawEllipse (juce::Rectangle<float> (pack * 2.0f, pack * 2.0f).withCentre (centre), 1.0f);

        const auto hub = r * 0.36f;
        juce::Path windows;
        for (int k = 0; k < 3; ++k)
        {
            const auto a0 = angle + (float) k * 2.0f * pi / 3.0f;
            windows.addPieSegment (juce::Rectangle<float> (hub * 2.0f, hub * 2.0f).withCentre (centre), a0, a0 + 1.2f, 0.45f);
        }
        g.setColour (screen::bg());
        g.fillEllipse (juce::Rectangle<float> (hub * 2.0f, hub * 2.0f).withCentre (centre));
        g.setColour (hot (0.2f).withAlpha (0.55f + 0.4f * engaged));
        g.fillPath (windows);
        g.setColour (screen::hud().withAlpha (0.5f));
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (centre));
    };
    reel (left, supply, reelAngle);
    reel (right, takeUp, reelAngleRight);

    // The speed, big.
    const auto text = speedText (speed);
    g.setColour ((std::abs (speed - 1.0f) < 0.03f ? screen::hud().withAlpha (0.75f) : hot (0.3f)).withMultipliedAlpha (0.4f + 0.6f * engaged));
    g.setFont (Fonts::mono (20.0f, true));
    g.drawText (text, s.withTrimmedTop (s.getHeight() - 34.0f).withTrimmedBottom (8.0f), juce::Justification::centred, false);

    screen::scanlines (g, s, 0.06f);
}

void TimeView::paintSpeed (juce::Graphics& g, juce::Rectangle<float> s)
{
    screen::paintScreen (g, s, "SPEED", 0.6f, this);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (s.reduced (1.0f).toNearestInt());

    const auto a = s.reduced (14.0f, 10.0f).withTrimmedTop (16.0f).withTrimmedLeft (18.0f);
    auto yOf = [&a] (float v) { return a.getBottom() - (juce::jlimit (-1.0f, 2.0f, v) + 1.0f) / 3.0f * a.getHeight(); };

    for (const auto [v, label] : { std::pair { 1.0f, "1x" }, std::pair { 0.0f, "0" } })
    {
        juce::Path line;
        line.startNewSubPath (a.getX(), yOf (v));
        line.lineTo (a.getRight(), yOf (v));
        juce::Path dashed;
        const float dashes[] = { 3.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
        g.setColour (screen::hud().withAlpha (v == 1.0f ? 0.22f : 0.12f));
        g.fillPath (dashed);
        hudText (g, label, { a.getX() - 22.0f, yOf (v) - 6.0f, 18.0f, 12.0f }, 0.4f, juce::Justification::centredRight);
    }

    // The last seconds, newest on the right.
    juce::Path trace;
    const auto n = (int) speedHistory.size();
    for (int i = 0; i < n; ++i)
    {
        const auto v = speedHistory[(size_t) ((speedWrite + i) % n)];
        const juce::Point<float> p (a.getX() + a.getWidth() * (float) i / (float) (n - 1), yOf (v));
        if (i == 0) trace.startNewSubPath (p); else trace.lineTo (p);
    }
    glowStroke (g, trace, speedColour (speed).withMultipliedAlpha (0.5f + 0.5f * engaged), 1.5f, 0.6f * (0.3f + engaged));
    screen::scanlines (g, s, 0.06f);
}

//==============================================================================
void TimeView::mouseMove (const juce::MouseEvent& e)
{
    idle.wake();
    hover = hitTest (e.position);
    setMouseCursor (hover.index >= 0 ? juce::MouseCursor::DraggingHandCursor
                                     : (laneAt (e.position) >= 0 ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor));
}

void TimeView::mouseExit (const juce::MouseEvent&)
{
    idle.wake();
    hover = {};
}

void TimeView::mouseDown (const juce::MouseEvent& e)
{
    const auto lane = laneAt (e.position);
    if (e.mods.isPopupMenu())
    {
        if (lane >= 0)
            showMenu (lane);
        return;
    }

    drag = hitTest (e.position);
    if (drag.index < 0 && lane >= 0)
    {
        // A new point where the mouse is (on the grid), dragged from here.
        auto slot = processor.getTimeSlot (shownSlot);
        auto& c = curveOf (slot, lane);
        if (c.numPoints >= TimeCurve::maxPoints)
            return;
        const auto p = fromScreen (lane, e.position, ! e.mods.isShiftDown());
        c.add (juce::jlimit (0.001f, 0.999f, p.x), p.y);
        c.sort();
        processor.getUndoManager().beginNewTransaction ("Add TIME point");
        store (slot);
        for (int i = 0; i < c.numPoints; ++i)
            if (std::abs (c.x[(size_t) i] - juce::jlimit (0.001f, 0.999f, p.x)) < 1.0e-6f && std::abs (c.y[(size_t) i] - p.y) < 1.0e-6f)
                drag = { lane, i };
        shown = slot;
        return;
    }
    if (drag.index >= 0)
        processor.getUndoManager().beginNewTransaction ("Move TIME point");
}

void TimeView::mouseDrag (const juce::MouseEvent& e)
{
    idle.wake();
    if (drag.index < 0)
        return;
    auto slot = processor.getTimeSlot (shownSlot);
    auto& c = curveOf (slot, drag.lane);
    if (drag.index >= c.numPoints)
        return;

    const auto p = fromScreen (drag.lane, e.position, ! e.mods.isShiftDown());
    const auto first = drag.index == 0, last = drag.index == c.numPoints - 1;
    // The ends stay at the ends of the cycle; the others between their neighbours.
    c.x[(size_t) drag.index] = first ? 0.0f : (last ? 1.0f : juce::jlimit (c.x[(size_t) drag.index - 1], c.x[(size_t) drag.index + 1], p.x));
    c.y[(size_t) drag.index] = p.y;
    store (slot);
    shown = slot;
}

void TimeView::mouseUp (const juce::MouseEvent&)
{
    if (drag.index >= 0)
        processor.getUndoManager().beginNewTransaction();
    drag = {};
}

void TimeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.position);
    auto slot = processor.getTimeSlot (shownSlot);
    auto& c = curveOf (slot, hit.lane < 0 ? 0 : hit.lane);
    if (hit.index <= 0 || hit.index >= c.numPoints - 1)
        return; // the ends stay
    for (int i = hit.index; i + 1 < c.numPoints; ++i)
    {
        c.x[(size_t) i] = c.x[(size_t) i + 1];
        c.y[(size_t) i] = c.y[(size_t) i + 1];
    }
    --c.numPoints;
    processor.getUndoManager().beginNewTransaction ("Remove TIME point");
    store (slot);
    processor.getUndoManager().beginNewTransaction();
    hover = drag = {};
}

void TimeView::store (const TimeSlot& slot)
{
    processor.setTimeSlot (shownSlot, slot);
    if (activeSlot (processor) < 0)
        processor.setParameterAsGesture (Params::timeSlot, (float) (shownSlot + 1));
}

void TimeView::showMenu (int lane)
{
    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String (lane == timeLane ? "TIME" : "VOLUME") + "  -  SLOT " + juce::String (shownSlot + 1));
    menu.addItem (lane == timeLane ? "Flat (no time effect)" : "Flat (full volume)", [this, lane]
    {
        auto slot = processor.getTimeSlot (shownSlot);
        curveOf (slot, lane) = TimeCurve::flat (lane == timeLane ? 0.0f : 1.0f);
        processor.getUndoManager().beginNewTransaction ("Flatten TIME curve");
        store (slot);
        processor.getUndoManager().beginNewTransaction();
    });
    menu.addItem ("Both curves flat (live, full volume)", [this]
    {
        processor.getUndoManager().beginNewTransaction ("Flatten TIME slot");
        store ({});
        processor.getUndoManager().beginNewTransaction();
    });
    menu.addItem ("Back to " + juce::String (TimeFactory::name (shownSlot)), processor.isTimeSlotEdited (shownSlot), false,
                  [this] { processor.resetTimeSlot (shownSlot); });
    menu.addSeparator();
    juce::PopupMenu copy, banks[TimeFactory::numSlots / TimeFactory::slotsPerBank];
    for (int k = 0; k < TimeFactory::numSlots; ++k)
        if (k != shownSlot)
            banks[k / TimeFactory::slotsPerBank].addItem (juce::String (k + 1) + "  " + TimeFactory::name (k), [this, k, lane]
            {
                auto slot = processor.getTimeSlot (shownSlot);
                const auto from = processor.getTimeSlot (k);
                curveOf (slot, lane) = lane == timeLane ? from.time : from.volume;
                processor.getUndoManager().beginNewTransaction ("Copy TIME curve");
                store (slot);
                processor.getUndoManager().beginNewTransaction();
            });
    for (int b = 0; b < TimeFactory::numSlots / TimeFactory::slotsPerBank; ++b) // one submenu per bank
        copy.addSubMenu (juce::String ("Bank ") + (char) ('A' + b), banks[b]);
    menu.addSubMenu ("Take the curve of", copy);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

//==============================================================================
TimeSlots::TimeSlots (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    clock.add (this);
}

TimeSlots::~TimeSlots()
{
    clock.remove (this);
}

juce::Rectangle<float> TimeSlots::cardBounds (int slot) const
{
    // The shown bank only: 20 cards in two rows.
    const auto i = slot - bank * TimeFactory::slotsPerBank;
    if (i < 0 || i >= TimeFactory::slotsPerBank)
        return {};
    constexpr int columns = TimeFactory::slotsPerBank / 2;
    constexpr float gap = 6.0f;
    const auto w = ((float) getWidth() - gap * (columns - 1)) / columns;
    const auto h = ((float) getHeight() - gap) * 0.5f;
    return { (float) (i % columns) * (w + gap), (float) (i / columns) * (h + gap), w, h };
}

int TimeSlots::cardAt (juce::Point<float> p) const
{
    for (int k = bank * TimeFactory::slotsPerBank; k < (bank + 1) * TimeFactory::slotsPerBank; ++k)
        if (cardBounds (k).contains (p))
            return k;
    return -1;
}

void TimeSlots::animationTick (double dt)
{
    const auto active = activeSlot (processor);
    // A slot switched on in another bank (automation, a preset, undo): show its bank.
    if (active != shownActive && active >= 0 && active / TimeFactory::slotsPerBank != bank)
    {
        setBank (active / TimeFactory::slotsPerBank);
        if (onBankChanged) onBankChanged (bank);
    }
    else if (shownEdit == -2 && active < 0 && timeEditSlot (processor) / TimeFactory::slotsPerBank != bank)
    {
        setBank (timeEditSlot (processor) / TimeFactory::slotsPerBank); // the first look: the bank of the slot shown above
        if (onBankChanged) onBankChanged (bank);
    }
    bool changed = active != shownActive || timeEditSlot (processor) != shownEdit;
    shownActive = active;
    shownEdit = timeEditSlot (processor);
    for (int k = 0; k < TimeFactory::numSlots; ++k)
    {
        const auto target = k == active ? 1.0f : 0.0f;
        if (std::abs (glow[(size_t) k] - target) > 0.001f)
        {
            glow[(size_t) k] += (target - glow[(size_t) k]) * std::min (1.0f, (float) dt * 9.0f);
            changed = true;
        }
    }
    // The active card pulses with the beat - only while something plays (otherwise: at rest).
    if (changed || (active >= 0 && processor.isProcessing() && (processor.getTimeShaper().isClockRunning() || processor.isSounding())))
        repaint();
}

void TimeSlots::paint (juce::Graphics& g)
{
    const auto active = activeSlot (processor);
    const auto edit = timeEditSlot (processor);
    const auto& shaper = processor.getTimeShaper();

    for (int k = bank * TimeFactory::slotsPerBank; k < (bank + 1) * TimeFactory::slotsPerBank; ++k)
    {
        const auto b = cardBounds (k).reduced (0.5f);
        const auto on = glow[(size_t) k];
        const auto isEdit = k == edit && active < 0;

        g.setColour ((k == hovered ? Colours::surfaceHover : Colours::surfaceRaised));
        g.fillRoundedRectangle (b, Metrics::radiusSmall);
        if (on > 0.001f)
        {
            g.setColour (Colours::accent.withAlpha (0.10f + 0.10f * on));
            g.fillRoundedRectangle (b, Metrics::radiusSmall);
        }
        g.setColour (on > 0.5f ? Colours::accent : (isEdit ? Colours::textDim : Colours::hairline));
        g.drawRoundedRectangle (b, Metrics::radiusSmall, on > 0.5f ? 1.4f : Metrics::hairline);

        auto inner = b.reduced (6.0f, 4.0f);
        auto top = inner.removeFromTop (12.0f);
        drawText (g, juce::String (k + 1).paddedLeft ('0', 2), top, TextStyle::micro, on > 0.5f ? Colours::accent : Colours::textDim);
        if (processor.isTimeSlotEdited (k))
        {
            g.setColour (Colours::textDim);
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ top.getRight() - 3.0f, top.getCentreY() }));
        }
        const auto nameArea = inner.removeFromBottom (12.0f);
        drawText (g, TimeFactory::name (k), nameArea, TextStyle::micro, on > 0.5f ? Colours::text : Colours::textDim);

        // The curves in miniature: TIME (live at the top), VOLUME faintly under it.
        const auto slot = processor.getTimeSlot (k);
        const auto pic = inner.reduced (0.0f, 2.0f);
        juce::Path vol, tim;
        for (int i = 0; i < slot.volume.numPoints; ++i)
        {
            const juce::Point<float> p (pic.getX() + slot.volume.x[(size_t) i] * pic.getWidth(), pic.getBottom() - slot.volume.y[(size_t) i] * pic.getHeight());
            if (i == 0) { vol.startNewSubPath (pic.getX(), pic.getBottom()); vol.lineTo (p); } else vol.lineTo (p);
        }
        vol.lineTo (pic.getRight(), pic.getBottom());
        vol.closeSubPath();
        g.setColour (nebulaGlow().withAlpha (0.10f + 0.08f * on));
        g.fillPath (vol);
        for (int i = 0; i < slot.time.numPoints; ++i)
        {
            const juce::Point<float> p (pic.getX() + slot.time.x[(size_t) i] * pic.getWidth(), pic.getY() + slot.time.y[(size_t) i] * pic.getHeight());
            if (i == 0) tim.startNewSubPath (p); else tim.lineTo (p);
        }
        g.setColour (Colours::accent.withAlpha (0.55f + 0.45f * on));
        g.strokePath (tim, juce::PathStrokeType (1.2f));

        // The active card: where the beat is.
        if (on > 0.01f && shaper.isEngaged())
        {
            const auto x = pic.getX() + shaper.getDisplayPosition() * pic.getWidth();
            g.setColour (Colours::accent.withAlpha (0.7f * on));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, pic.getY(), 1.0f, pic.getHeight()));
        }
    }
}

void TimeSlots::mouseMove (const juce::MouseEvent& e)
{
    const auto k = cardAt (e.position);
    setMouseCursor (k >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (k != hovered) { hovered = k; repaint(); }
}

void TimeSlots::mouseExit (const juce::MouseEvent&)
{
    if (hovered != -1) { hovered = -1; repaint(); }
}

void TimeSlots::mouseDown (const juce::MouseEvent& e)
{
    const auto k = cardAt (e.position);
    if (k < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("SLOT " + juce::String (k + 1) + "  " + TimeFactory::name (k));
        menu.addItem ("Edit (without switching it on)", [this, k] { setTimeEditSlot (processor, k); });
        menu.addItem ("Back to the factory shape", processor.isTimeSlotEdited (k), false, [this, k] { processor.resetTimeSlot (k); });
        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }

    clickSlot (k);
}

void TimeSlots::clickSlot (int k)
{
    // This slot on (and shown). The active one again: everything on the TIME page back to
    // how it started - its TIME and VOLUME curves, MIX and the cycle length (it stays on).
    setTimeEditSlot (processor, k);
    if (activeSlot (processor) == k)
    {
        processor.resetTimeSettings (k);
        processor.setStatusMessage ("TIME reset  -  Ctrl+Z brings your settings back");
    }
    else
        processor.setParameterAsGesture (Params::timeSlot, (float) (k + 1));
}

} // namespace kerf::ui
