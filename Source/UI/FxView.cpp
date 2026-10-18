#include "FxView.h"
#include "ScreenStyle.h"

namespace kerf::ui
{

namespace
{
    using screen::signal;
    using screen::hot;
    using screen::hudText;
    using screen::glowStroke;

    const char* dirtModes[] = { "WARM", "FUZZ", "CRUSH" };
    const char* wobbleModes[] = { "TAPE", "CHORUS" };
    const char* pumpModes[] = { "1/4", "1/8", "1/16" };

    juce::String percent (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }

    juce::String hzShort (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "K" : juce::String (juce::roundToInt (hz));
    }

    /** Position (0..1) of a frequency on the spectrum's log axis. */
    float freqToU (float hz)
    {
        const auto f0 = SpectrumAnalyzer::bandFrequency (0), f1 = SpectrumAnalyzer::bandFrequency (SpectrumAnalyzer::numBands - 1);
        return juce::jlimit (0.0f, 1.0f, std::log (hz / f0) / std::log (f1 / f0));
    }
}

FxView::FxView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    static_assert (bins == SpectrumAnalyzer::numBands, "one ridge point per spectrum band");
    clock.add (this);
    setInterceptsMouseClicks (false, false);
}

FxView::~FxView()
{
    clock.remove (this);
}

void FxView::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (26.0f);
    r.removeFromTop (2.0f);

    auto modules = r.removeFromRight (318.0f);
    r.removeFromRight (10.0f);
    chainScreen = r;

    spaceScreen = modules.removeFromTop (modules.getHeight() * 0.3f);
    modules.removeFromTop (8.0f);
    auto row1 = modules.removeFromTop ((modules.getHeight() - 8.0f) * 0.5f);
    modules.removeFromTop (8.0f);
    auto row2 = modules;
    const auto half = (row1.getWidth() - 8.0f) * 0.5f;
    dirtScreen = row1.removeFromLeft (half);
    toneScreen = row1.withTrimmedLeft (8.0f);
    wobbleScreen = row2.removeFromLeft (half);
    pumpScreen = row2.withTrimmedLeft (8.0f);
}

FxView::Settings FxView::readSettings() const
{
    auto value = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };
    Settings st;
    st.space = value (Params::space);
    st.dirt = value (Params::drive);
    st.dirtMode = juce::jlimit (0, 2, (int) value (Params::dirtMode));
    st.filter = value (Params::filter);
    st.resonance = value (Params::resonance);
    st.wobble = value (Params::wobble);
    st.wobbleMode = juce::jlimit (0, 1, (int) value (Params::wobbleMode));
    st.pump = value (Params::pump);
    st.pumpMode = juce::jlimit (0, 2, (int) value (Params::pumpMode));
    const auto delay = PingPongDelay::fromMacro (st.space, processor.getEffectiveTempo());
    st.delaySeconds = std::max (0.05, delay.delaySeconds);
    st.feedback = delay.feedback;
    return st;
}

//==============================================================================
void FxView::animationTick (double dt)
{
    if (! isVisible())
        return;

    time += dt;
    if (intro < 1.0f)
        intro = std::min (1.0f, intro + (float) dt / 0.75f);

    s = readSettings();

    // Output spectrum -> the newest ridge (fast rise, slower fall).
    const auto& bands = processor.getSpectrum().getBands();
    for (int b = 0; b < bins; ++b)
    {
        // Quiet bands sink to the floor; only real energy makes mountains.
        const auto v = std::pow (juce::jlimit (0.0f, 1.0f, (bands[(size_t) b] + 66.0f) / 60.0f), 1.6f);
        current[(size_t) b] += (v - current[(size_t) b]) * (v > current[(size_t) b] ? 0.6f : 0.25f);
    }

    // Output level and the latest sample (for the DIRT dot).
    {
        auto& tap = processor.getOutputTap();
        tap.pull();
        float peak = 0.0f;
        for (int i = 0; i < 512; ++i)
            peak = std::max ({ peak, std::abs (tap.getLeft (i)), std::abs (tap.getRight (i)) });
        level = peak > level ? peak : level + (peak - level) * std::min (1.0f, (float) dt * 5.0f);
        sample = 0.5f * (tap.getLeft (0) + tap.getRight (0));
    }

    // A new row every other frame (30 rows a second): the landscape moves back.
    if (++tickCount % 2 == 0)
    {
        newestRow = (newestRow + 1) % rows;
        history[(size_t) newestRow] = current;
    }

    // WOBBLE and PUMP move while something plays (the host, the pattern, a chop or the
    // sample) and hold where they are when it stops. Playback starting again: they start from
    // the top (PUMP follows the effect's own clock, which starts its cycle from the top too).
    bool playing = processor.isHostPlaying() || processor.isPatternPlaying();
    for (const auto& v : processor.getEngine().getVoiceDisplay())
        playing = playing || v.chopId.load (std::memory_order_relaxed) != 0;
    if (playing)
    {
        if (! wasPlaying)
            motion = 0.0;
        motion += dt;
        pumpPpq = processor.getPumpPpq();
    }
    wasPlaying = playing;
    pumpGain = PumpProcessor::gainAt (pumpPpq, processor.getEffectiveTempo(), { s.pump, (PumpProcessor::Mode) s.pumpMode });

    // At rest (silence, the landscape run empty, nothing changed): no repaint.
    const auto moving = playing && (s.wobble > 0.001f || s.pump > 0.001f);
    if (idle.update (dt, intro < 1.0f || processor.isSounding() || moving, processor.getParameterChangeCount()))
        repaint();
}

float FxView::shapeHeight (float h, int) const
{
    const auto a = s.dirt;
    if (a < 0.001f)
        return h;

    switch (s.dirtMode)
    {
        case 0: // WARM: soft saturation, everything gets fuller
        {
            const auto k = 1.0f + 3.0f * a;
            return std::tanh (k * h) / std::tanh (k);
        }
        case 1: // FUZZ: driven into a hard ceiling
            return std::min (h * (1.0f + 2.0f * a), 1.0f - 0.5f * a);
        default: // CRUSH: few levels
        {
            const auto levels = std::max (3.0f, std::round (16.0f - 12.0f * a));
            return std::floor (h * levels + 0.5f) / levels;
        }
    }
}

//==============================================================================
void FxView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat(), Backdrop::deep);

    // Header: the chain at a glance.
    {
        auto h = header.reduced (12.0f, 0.0f);
        drawText (g, "FX", h.removeFromLeft (40.0f), TextStyle::sectionLabel, Colours::text);

        juce::String readout;
        readout << "SPACE " << percent (s.space)
                << "   DIRT " << dirtModes[s.dirtMode] << " " << percent (s.dirt)
                << "   TONE " << (s.filter < -0.005f ? "LP " + hzShort (ToneProcessor::lowPassCutoff (s.filter))
                                                    : (s.filter > 0.005f ? "HP " + hzShort (ToneProcessor::highPassCutoff (s.filter)) : juce::String ("OFF")))
                << "   WOBBLE " << wobbleModes[s.wobbleMode] << " " << percent (s.wobble)
                << "   PUMP " << pumpModes[s.pumpMode] << " " << percent (s.pump);
        drawText (g, readout, h, TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }

    paintChain (g, chainScreen);
    paintSpace (g, spaceScreen);
    paintDirt (g, dirtScreen);
    paintTone (g, toneScreen);
    paintWobble (g, wobbleScreen);
    paintPump (g, pumpScreen);

    // Opening animation: the screens switch on one after another.
    if (intro < 1.0f)
    {
        const juce::Rectangle<float> screens[] = { chainScreen, spaceScreen, dirtScreen, toneScreen, wobbleScreen, pumpScreen };
        for (int i = 0; i < 6; ++i)
            screen::powerOn (g, screens[i], intro * 1.6f - 0.1f * (float) i);
    }
}

void FxView::moduleFrame (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, const juce::String& value, bool on) const
{
    screen::paintScreen (g, area, label, on ? 0.7f : 0.35f, this);
    hudText (g, on ? value : juce::String ("OFF"), area.reduced (12.0f, 8.0f).withTrimmedTop (area.getHeight() - 28.0f),
             on ? 0.6f : 0.3f, juce::Justification::centredRight);
}

//==============================================================================
void FxView::paintChain (juce::Graphics& g, juce::Rectangle<float> area)
{
    screen::paintScreen (g, area, "FX CHAIN", 0.6f, this);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    const auto m = area.reduced (18.0f, 14.0f).withTrimmedTop (10.0f);
    const auto horizon = m.getY() + m.getHeight() * 0.16f;
    const auto front = m.getBottom() - 18.0f;
    const auto vp = m.getCentreX();
    const auto backScale = 0.52f;

    auto rowY = [&] (float t) { return front + (horizon - front) * std::pow (t, 0.8f); };
    auto rowScale = [&] (float t) { return 1.0f + (backScale - 1.0f) * t; };
    auto xAt = [&] (float u, float t) { return vp + ((m.getX() + u * m.getWidth()) - vp) * rowScale (t); };
    const auto ampFront = (front - horizon) * 0.62f;

    // WOBBLE: the landscape wavers (TAPE slow and uneven, CHORUS even).
    auto wobbleAt = [&] (int rowAge)
    {
        if (s.wobble < 0.001f)
            return 0.0f;
        const auto t = motion - rowAge / 30.0;
        const auto w = s.wobbleMode == 0 ? std::sin (t * 1.4) + 0.45 * std::sin (t * 3.7 + 1.0) + 0.2 * std::sin (t * 9.1)
                                         : std::sin (t * 5.0);
        return (float) w * 9.0f * s.wobble;
    };

    // CRUSH also steps the ridges sideways.
    const auto step = s.dirt > 0.001f && s.dirtMode == 2 ? 1 + juce::roundToInt (6.0f * s.dirt) : 1;
    const auto heat = s.dirt > 0.001f && s.dirtMode == 0 ? 0.35f * s.dirt : 0.0f;

    // Floor grid (converging lines + depth lines)
    g.setColour (screen::hud().withAlpha (0.05f));
    for (int k = 0; k <= 16; ++k)
    {
        const auto u = (float) k / 16.0f;
        g.drawLine (xAt (u, 0.0f), front, xAt (u, 1.0f), horizon, 1.0f);
    }
    for (int k = 0; k <= 6; ++k)
    {
        const auto t = (float) k / 6.0f;
        g.drawLine (xAt (0.0f, t), rowY (t), xAt (1.0f, t), rowY (t), 1.0f);
    }

    // PUMP: on every cycle a pulse of light rolls from the front to the horizon over the
    // floor, fading as it goes (the swell after each dip).
    const auto pumping = s.pump > 0.001f;
    const auto pumpPhase = pumping ? (float) (pumpPpq / PumpProcessor::cycleBeats ((PumpProcessor::Mode) s.pumpMode)
                                              - std::floor (pumpPpq / PumpProcessor::cycleBeats ((PumpProcessor::Mode) s.pumpMode))) : 0.0f;
    if (pumping)
    {
        const auto t = std::pow (pumpPhase, 0.6f);
        const auto y = rowY (t);
        const auto strength = (1.0f - pumpPhase) * (0.35f + 0.65f * s.pump);
        juce::Path pulse;
        pulse.startNewSubPath (xAt (0.0f, t), y);
        pulse.lineTo (xAt (1.0f, t), y);
        glowStroke (g, pulse, signal().withAlpha (0.8f * strength), 1.4f, strength);

        // A glow on the floor behind the pulse.
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.12f * strength), vp, y,
                                                 signal().withAlpha (0.0f), vp, front, false));
        juce::Path floor;
        floor.startNewSubPath (xAt (0.0f, t), y);
        floor.lineTo (xAt (1.0f, t), y);
        floor.lineTo (xAt (1.0f, 0.0f), front);
        floor.lineTo (xAt (0.0f, 0.0f), front);
        floor.closeSubPath();
        g.fillPath (floor);
    }

    auto ridgePath = [&] (const std::array<float, bins>& row, float t, float dx, juce::Path& ridge, juce::Path* solid)
    {
        const auto scale = rowScale (t);
        const auto base = rowY (t);
        for (int b = 0; b < bins; ++b)
        {
            const auto src = b - (b % step);
            const auto h = shapeHeight (row[(size_t) src], b);
            const auto x = xAt ((float) b / (float) (bins - 1), t) + dx * scale;
            const auto y = base - h * ampFront * scale;
            if (b == 0)
            {
                ridge.startNewSubPath (x, y);
                if (solid != nullptr) { solid->startNewSubPath (x, base + 1.5f); solid->lineTo (x, y); }
            }
            else
            {
                ridge.lineTo (x, y);
                if (solid != nullptr) solid->lineTo (x, y);
            }
        }
        if (solid != nullptr)
        {
            solid->lineTo (xAt (1.0f, t) + dx * scale, base + 1.5f);
            solid->closeSubPath();
        }
    };

    // Ridges, back to front; each one hides what is behind it. With PUMP the nearest ones
    // dim on the dip and glow up on the swell.
    const auto linger = 1.7f;
    const auto breath = pumping ? 0.35f + 0.65f * pumpGain : 1.0f;
    for (int k = rows - 1; k >= 0; --k)
    {
        const auto t = (float) k / (float) (rows - 1);
        const auto& row = history[(size_t) ((newestRow - k + rows) % rows)];
        juce::Path ridge, solid;
        ridgePath (row, t, wobbleAt (k), ridge, &solid);

        g.setColour (screen::bg());
        g.fillPath (solid);

        const auto nearness = std::pow (1.0f - t, linger);
        const auto lit = 1.0f - (1.0f - breath) * nearness; // the front breathes most
        const auto colour = screen::heat (signal(), heat + (pumping ? 0.25f * (pumpGain - 0.5f) * nearness : 0.0f))
                                    .withAlpha ((0.10f + 0.85f * nearness) * lit);
        glowStroke (g, ridge, colour, k == 0 ? 1.8f : 1.0f, k < 3 ? (1.0f - t) * (0.6f + heat) * lit : 0.0f);

        // CHORUS: a second, shifted image of the nearest ridges.
        if (k < 3 && s.wobble > 0.001f && s.wobbleMode == 1)
        {
            juce::Path twin;
            ridgePath (row, t, -wobbleAt (k), twin, nullptr);
            g.setColour (signal().withAlpha (0.35f * s.wobble * (1.0f - t)));
            g.strokePath (twin, juce::PathStrokeType (1.0f));
        }
    }

    // SPACE: ghost echoes of the front ridge, further back each time, bouncing left / right.
    if (s.space > 0.001f)
    {
        const auto rowsPerEcho = (float) s.delaySeconds * 30.0f;
        auto strength = 1.0f;
        for (int e = 1; e <= 5; ++e)
        {
            strength *= std::max (0.3f, s.feedback);
            const auto depth = std::min (0.92f, (float) e * rowsPerEcho / (float) (rows - 1));
            const auto side = (e % 2 == 1 ? -1.0f : 1.0f) * (10.0f + 8.0f * (float) e) * s.space;
            juce::Path ghost;
            ridgePath (current, depth, side, ghost, nullptr);
            glowStroke (g, ghost, hot (0.25f).withAlpha (0.65f * s.space * strength), 1.0f, 0.5f * strength);
        }
    }

    // TONE: a wall of light at the cutoff; the removed side is darkened; resonance peaks.
    if (std::abs (s.filter) > 0.005f)
    {
        const auto lowPass = s.filter < 0.0f;
        const auto cutoff = lowPass ? ToneProcessor::lowPassCutoff (s.filter) : ToneProcessor::highPassCutoff (s.filter);
        const auto uc = freqToU (cutoff);
        const auto edge = lowPass ? 1.0f : 0.0f;
        const auto strength = std::sqrt (std::abs (s.filter));

        juce::Path shade;
        shade.startNewSubPath (xAt (uc, 0.0f), front + 18.0f);
        shade.lineTo (xAt (edge, 0.0f) + (lowPass ? 30.0f : -30.0f), front + 18.0f);
        shade.lineTo (xAt (edge, 1.0f) + (lowPass ? 30.0f : -30.0f), m.getY());
        shade.lineTo (xAt (uc, 1.0f), m.getY());
        shade.closeSubPath();
        g.setColour (screen::bg().withAlpha (0.55f * strength));
        g.fillPath (shade);

        juce::Path wall;
        wall.startNewSubPath (xAt (uc, 0.0f), front);
        wall.lineTo (xAt (uc, 1.0f), horizon);
        glowStroke (g, wall, signal().withAlpha (0.7f), 1.2f, 0.8f);

        const auto x = xAt (uc, 0.0f);
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.0f), x, front - ampFront, signal().withAlpha (0.3f * strength), x, front, false));
        g.fillRect (juce::Rectangle<float> (x - 4.0f, front - ampFront, 8.0f, ampFront));

        if (s.resonance > 0.05f)
        {
            const auto b = juce::jlimit (0, bins - 1, juce::roundToInt (uc * (bins - 1)));
            const auto y = front - shapeHeight (current[(size_t) b], b) * ampFront - s.resonance * 22.0f;
            const auto r = 2.0f + 5.0f * s.resonance;
            g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.5f), x, y, signal().withAlpha (0.0f), x + r * 4.0f, y, true));
            g.fillEllipse (juce::Rectangle<float> (r * 8.0f, r * 8.0f).withCentre ({ x, y }));
            g.setColour (hot (0.6f));
            g.fillEllipse (juce::Rectangle<float> (r * 1.4f, r * 1.4f).withCentre ({ x, y }));
        }
    }

    // FUZZ: sparks where the front ridge hits the ceiling.
    if (s.dirt > 0.05f && s.dirtMode == 1)
    {
        const auto ceiling = 1.0f - 0.5f * s.dirt;
        for (int b = 0; b < bins; b += 2)
            if (current[(size_t) b] * (1.0f + 2.0f * s.dirt) > ceiling && random.nextFloat() < 0.35f)
            {
                const auto x = xAt ((float) b / (float) (bins - 1), 0.0f) + wobbleAt (0);
                const auto y = front - ceiling * ampFront - 2.0f - random.nextFloat() * 6.0f;
                g.setColour (hot (0.7f).withAlpha (0.4f + 0.6f * random.nextFloat()));
                g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre ({ x, y }));
            }
    }

    screen::scanlines (g, area);
    hudText (g, "LOW", { m.getX(), front + 4.0f, 60.0f, 12.0f }, 0.35f);
    hudText (g, "HIGH", { m.getRight() - 60.0f, front + 4.0f, 60.0f, 12.0f }, 0.35f, juce::Justification::centredRight);

    if (level < 0.0005f)
    {
        const auto blink = std::fmod (time, 1.2) < 0.7;
        hudText (g, "PLAY SOMETHING", area.withTrimmedTop (22.0f).withHeight (14.0f), blink ? 0.5f : 0.2f, juce::Justification::centred);
    }
}

//==============================================================================
void FxView::paintSpace (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto on = s.space > 0.001f;
    moduleFrame (g, area, "SPACE", percent (s.space) + "  " + juce::String (juce::roundToInt (s.delaySeconds * 1000.0)) + " MS", on);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    // Ping-pong as a bouncing ball: it flies between the left and the right speaker, one
    // echo time per flight, each arc lower by the feedback (an echo losing energy). Every
    // landing rings; the arcs already flown glow on.
    const auto m = area.reduced (16.0f, 10.0f).withTrimmedTop (14.0f).withTrimmedBottom (22.0f);
    const auto floorY = m.getBottom();
    const juce::Point<float> left (m.getX() + 14.0f, floorY), right (m.getRight() - 14.0f, floorY);

    g.setColour (screen::hud().withAlpha (0.08f));
    g.fillRect (juce::Rectangle<float> (m.getX(), floorY, m.getWidth(), 1.0f));

    auto speaker = [&] (juce::Point<float> p, const char* name, float glow)
    {
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.45f * glow), p.x, p.y, signal().withAlpha (0.0f), p.x + 16.0f, p.y, true));
        g.fillEllipse (juce::Rectangle<float> (32.0f, 32.0f).withCentre (p));
        g.setColour (on ? hot (0.2f + 0.6f * glow) : screen::hud().withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (7.0f, 3.0f).withCentre (p));
        const auto side = p.x < m.getCentreX() ? -16.0f : 4.0f; // beside the speaker, outwards
        hudText (g, name, { p.x + side, p.y - 5.0f, 12.0f, 10.0f }, 0.45f, juce::Justification::centred);
    };

    if (! on)
    {
        speaker (left, "L", 0.0f);
        speaker (right, "R", 0.0f);
        return;
    }

    constexpr int bounces = 6;
    const auto period = s.delaySeconds;
    const auto t = std::fmod (time, period * bounces);
    const auto flight = juce::jlimit (0, bounces - 1, (int) (t / period));
    const auto progress = (float) ((t - flight * period) / period);
    const auto fb = std::max (0.35f, s.feedback);
    const auto top = m.getHeight() * (0.55f + 0.45f * s.space);

    auto arcPoint = [&] (int k, float p)
    {
        const auto from = k % 2 == 0 ? left : right, to = k % 2 == 0 ? right : left;
        const auto height = top * std::pow (fb, (float) k);
        return juce::Point<float> (from.x + (to.x - from.x) * p, floorY - 4.0f * height * p * (1.0f - p));
    };

    // Arcs already flown glow on and fade.
    for (int k = 0; k < flight; ++k)
    {
        juce::Path arc;
        for (int i = 0; i <= 40; ++i)
        {
            const auto p = arcPoint (k, (float) i / 40.0f);
            if (i == 0) arc.startNewSubPath (p);
            else        arc.lineTo (p);
        }
        const auto age = (float) (flight - k - 1) + progress;
        const auto alpha = std::pow (fb, (float) k) * std::exp (-1.3f * age);
        glowStroke (g, arc, signal().withAlpha (0.12f + 0.5f * alpha), 1.0f, 0.6f * alpha);
    }

    // The ball in flight leaves a comet tail: brighter and thicker towards the ball.
    {
        const auto tailStart = std::max (0.0f, progress - 0.45f);
        constexpr int segments = 14;
        for (int i = 0; i < segments; ++i)
        {
            const auto a = tailStart + (progress - tailStart) * (float) i / segments;
            const auto b = tailStart + (progress - tailStart) * (float) (i + 1) / segments;
            const auto w = (float) (i + 1) / segments; // 0 = end of the tail, 1 = at the ball
            juce::Path seg;
            seg.startNewSubPath (arcPoint (flight, a));
            seg.lineTo (arcPoint (flight, b));
            glowStroke (g, seg, hot (0.2f * w).withAlpha (0.15f + 0.85f * w), 0.6f + 1.6f * w, w);
        }
    }

    // Landing rings.
    float leftGlow = 0.0f, rightGlow = 0.0f;
    for (int k = 0; k < flight; ++k)
    {
        const auto since = (float) (t - (k + 1) * period);
        const auto fade = std::max (0.0f, 1.0f - since / 0.7f) * std::pow (fb, (float) k);
        if (fade <= 0.0f)
            continue;
        const auto at = k % 2 == 0 ? right : left;
        (k % 2 == 0 ? rightGlow : leftGlow) = std::max (k % 2 == 0 ? rightGlow : leftGlow, fade);
        const auto radius = 4.0f + since * 46.0f;
        g.setColour (signal().withAlpha (0.7f * fade));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 0.7f).withCentre (at), 1.2f);
    }
    if (flight == 0 && progress < 0.15f)
        leftGlow = 1.0f - progress / 0.15f; // the sound leaves the left speaker

    speaker (left, "L", leftGlow);
    speaker (right, "R", rightGlow);

    // The ball (the echo) with its bloom.
    const auto ball = arcPoint (flight, progress);
    const auto strength = std::pow (fb, (float) flight);
    const auto r = 3.0f + 3.0f * strength;
    g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.55f * strength), ball.x, ball.y, signal().withAlpha (0.0f), ball.x + r * 4.0f, ball.y, true));
    g.fillEllipse (juce::Rectangle<float> (r * 8.0f, r * 8.0f).withCentre (ball));
    g.setColour (hot (0.5f + 0.4f * strength));
    g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (ball));

    screen::scanlines (g, area);
}

void FxView::paintDirt (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto on = s.dirt > 0.001f;
    moduleFrame (g, area, "DIRT", juce::String (dirtModes[s.dirtMode]) + " " + percent (s.dirt), on);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    const auto side = std::min (area.getWidth() - 28.0f, area.getHeight() - 44.0f);
    const auto m = juce::Rectangle<float> (side, side).withCentre ({ area.getCentreX(), area.getCentreY() + 2.0f });
    g.setColour (screen::hud().withAlpha (0.08f));
    g.fillRect (juce::Rectangle<float> (m.getX(), m.getCentreY(), m.getWidth(), 1.0f));
    g.fillRect (juce::Rectangle<float> (m.getCentreX(), m.getY(), 1.0f, m.getHeight()));

    // Transfer curve: input across, output up.
    auto transfer = [this] (float x)
    {
        const auto a = s.dirt;
        if (a < 0.001f)
            return x;
        switch (s.dirtMode)
        {
            case 0: { const auto k = 1.0f + 4.0f * a; return std::tanh (k * x) / std::tanh (k); }
            case 1: { const auto c = 1.0f - 0.35f * a; return juce::jlimit (-c, c, x * (1.0f + 6.0f * a)) / c * (0.75f + 0.25f * (1.0f - a)); }
            default: { const auto levels = std::max (2.0f, std::round (std::pow (2.0f, 6.0f - 4.5f * a))); return std::round (x * levels) / levels; }
        }
    };

    juce::Path curve;
    for (int i = 0; i <= 80; ++i)
    {
        const auto x = -1.0f + 2.0f * (float) i / 80.0f;
        const auto px = m.getCentreX() + x * m.getWidth() * 0.5f;
        const auto py = m.getCentreY() - juce::jlimit (-1.0f, 1.0f, transfer (x)) * m.getHeight() * 0.5f;
        if (i == 0) curve.startNewSubPath (px, py);
        else        curve.lineTo (px, py);
    }
    glowStroke (g, curve, on ? signal() : screen::hud().withAlpha (0.25f), on ? 1.5f : 1.0f, on ? 0.8f : 0.0f);

    // The signal riding the curve.
    if (on && level > 0.0005f)
    {
        const auto x = juce::jlimit (-1.0f, 1.0f, sample / std::max (0.05f, level));
        const juce::Point<float> p (m.getCentreX() + x * m.getWidth() * 0.5f, m.getCentreY() - transfer (x) * m.getHeight() * 0.5f);
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.5f), p.x, p.y, signal().withAlpha (0.0f), p.x + 10.0f, p.y, true));
        g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre (p));
        g.setColour (hot (0.6f));
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (p));
    }
    screen::scanlines (g, area);
}

void FxView::paintTone (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto on = std::abs (s.filter) > 0.005f;
    const auto lowPass = s.filter < 0.0f;
    const auto cutoff = lowPass ? ToneProcessor::lowPassCutoff (s.filter) : ToneProcessor::highPassCutoff (s.filter);
    moduleFrame (g, area, "TONE", (lowPass ? "LP " : "HP ") + hzShort (cutoff), on);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    const auto m = area.reduced (12.0f, 10.0f).withTrimmedTop (16.0f).withTrimmedBottom (14.0f);
    auto dbToY = [&m] (float db) { return m.getY() + m.getHeight() * (1.0f - (juce::jlimit (-36.0f, 18.0f, db) + 36.0f) / 54.0f); };

    // Live spectrum, faint.
    {
        juce::Path area2;
        area2.startNewSubPath (m.getX(), m.getBottom());
        for (int b = 0; b < bins; ++b)
            area2.lineTo (m.getX() + m.getWidth() * (float) b / (float) (bins - 1), m.getBottom() - current[(size_t) b] * m.getHeight() * 0.8f);
        area2.lineTo (m.getRight(), m.getBottom());
        area2.closeSubPath();
        g.setColour (screen::hud().withAlpha (0.06f));
        g.fillPath (area2);
    }

    g.setColour (screen::hud().withAlpha (0.08f));
    g.fillRect (juce::Rectangle<float> (m.getX(), dbToY (0.0f), m.getWidth(), 1.0f));

    // Response of the 12 dB filter (with resonance).
    const auto q = 0.707f + juce::jlimit (0.0f, 1.0f, s.resonance) * 5.5f;
    const auto f0 = SpectrumAnalyzer::bandFrequency (0), f1 = SpectrumAnalyzer::bandFrequency (bins - 1);
    juce::Path curve;
    for (int i = 0; i <= 90; ++i)
    {
        const auto u = (float) i / 90.0f;
        const auto f = f0 * std::pow (f1 / f0, u);
        auto db = 0.0f;
        if (on)
        {
            const auto r = lowPass ? f / cutoff : cutoff / f;
            db = -10.0f * std::log10 (std::pow (1.0f - r * r, 2.0f) + std::pow (r / q, 2.0f));
        }
        const auto x = m.getX() + u * m.getWidth();
        if (i == 0) curve.startNewSubPath (x, dbToY (db));
        else        curve.lineTo (x, dbToY (db));
    }
    glowStroke (g, curve, on ? signal() : screen::hud().withAlpha (0.25f), on ? 1.5f : 1.0f, on ? 0.8f : 0.0f);

    if (on)
    {
        const auto x = m.getX() + freqToU (cutoff) * m.getWidth();
        g.setColour (signal().withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, m.getY(), 1.0f, m.getHeight()));
    }
    screen::scanlines (g, area);
}

void FxView::paintWobble (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto on = s.wobble > 0.001f;
    moduleFrame (g, area, "WOBBLE", juce::String (wobbleModes[s.wobbleMode]) + " " + percent (s.wobble), on);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    const auto m = area.reduced (12.0f, 10.0f).withTrimmedTop (16.0f).withTrimmedBottom (14.0f);
    const auto cy = m.getCentreY();
    g.setColour (screen::hud().withAlpha (0.08f));
    g.fillRect (juce::Rectangle<float> (m.getX(), cy, m.getWidth(), 1.0f));

    // The modulation scrolling past (3 seconds): TAPE wow and flutter, CHORUS two voices.
    auto lfo = [this] (double t, float voice)
    {
        if (s.wobbleMode == 0)
            return (float) (0.6 * std::sin (t * 2.0 * juce::MathConstants<double>::pi * 0.55) + 0.3 * std::sin (t * 10.7 + 1.0) + 0.12 * std::sin (t * 41.0));
        return (float) std::sin (t * 2.0 * juce::MathConstants<double>::pi * 0.9 + voice * juce::MathConstants<double>::pi);
    };
    for (int voice = 0; voice < (s.wobbleMode == 1 ? 2 : 1); ++voice)
    {
        juce::Path trace;
        for (int i = 0; i <= 120; ++i)
        {
            const auto u = (float) i / 120.0f;
            const auto t = motion - 3.0 * (1.0 - u);
            const auto y = cy - lfo (t, (float) voice) * (on ? s.wobble : 0.0f) * m.getHeight() * 0.45f;
            if (i == 0) trace.startNewSubPath (m.getX() + u * m.getWidth(), y);
            else        trace.lineTo (m.getX() + u * m.getWidth(), y);
        }
        glowStroke (g, trace, on ? signal().withAlpha (voice == 0 ? 1.0f : 0.5f) : screen::hud().withAlpha (0.25f), on ? 1.4f : 1.0f, on ? 0.7f : 0.0f);
    }

    if (on)
    {
        const auto y = cy - lfo (motion, 0.0f) * s.wobble * m.getHeight() * 0.45f;
        g.setColour (hot (0.5f));
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ m.getRight(), y }));
    }
    screen::scanlines (g, area);
}

void FxView::paintPump (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto on = s.pump > 0.001f;
    moduleFrame (g, area, "PUMP", juce::String (pumpModes[s.pumpMode]) + " " + percent (s.pump), on);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    const auto m = area.reduced (12.0f, 10.0f).withTrimmedTop (16.0f).withTrimmedBottom (14.0f);
    const PumpProcessor::Settings settings { on ? s.pump : 0.0f, (PumpProcessor::Mode) s.pumpMode };
    const auto bpm = processor.getEffectiveTempo();

    // One bar: four beats, a tick on each.
    for (int beat = 0; beat <= 4; ++beat)
    {
        const auto x = m.getX() + m.getWidth() * (float) beat / 4.0f;
        g.setColour (screen::hud().withAlpha (beat % 4 == 0 ? 0.14f : 0.08f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, m.getBottom() - 4.0f, 1.0f, 4.0f));
    }
    g.setColour (screen::hud().withAlpha (0.08f));
    g.fillRect (juce::Rectangle<float> (m.getX(), m.getBottom(), m.getWidth(), 1.0f));

    // The level over the bar: a dip on every cycle, swelling back.
    auto yFor = [&m] (float gain) { return m.getBottom() - gain * m.getHeight(); };
    juce::Path curve, fill;
    fill.startNewSubPath (m.getX(), m.getBottom());
    constexpr int points = 160;
    for (int i = 0; i <= points; ++i)
    {
        const auto u = (float) i / (float) points;
        const auto y = yFor (PumpProcessor::gainAt (u * 4.0, bpm, settings));
        const auto x = m.getX() + u * m.getWidth();
        if (i == 0) curve.startNewSubPath (x, y);
        else        curve.lineTo (x, y);
        fill.lineTo (x, y);
    }
    fill.lineTo (m.getRight(), m.getBottom());
    fill.closeSubPath();

    if (on)
    {
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.2f), m.getX(), m.getY(), signal().withAlpha (0.02f), m.getX(), m.getBottom(), false));
        g.fillPath (fill);
    }
    glowStroke (g, curve, on ? signal() : screen::hud().withAlpha (0.25f), on ? 1.4f : 1.0f, on ? 0.6f : 0.0f);

    if (! on)
        return;

    // The beat running along the bar, riding the curve; it flares on every dip.
    const auto barPos = (float) (pumpPpq / 4.0 - std::floor (pumpPpq / 4.0));
    const auto x = m.getX() + barPos * m.getWidth();
    const juce::Point<float> dot (x, yFor (pumpGain));
    g.setColour (signal().withAlpha (0.3f));
    g.fillRect (juce::Rectangle<float> (x - 0.5f, m.getY(), 1.0f, m.getHeight()));
    const auto flare = 1.0f - pumpGain;
    const auto r = 3.0f + 4.0f * flare;
    g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.5f), dot.x, dot.y, signal().withAlpha (0.0f), dot.x + r * 4.0f, dot.y, true));
    g.fillEllipse (juce::Rectangle<float> (r * 8.0f, r * 8.0f).withCentre (dot));
    g.setColour (hot (0.4f + 0.5f * flare));
    g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (dot));
    screen::scanlines (g, area);
}

} // namespace kerf::ui
