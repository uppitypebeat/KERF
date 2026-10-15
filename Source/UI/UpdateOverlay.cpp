#include "UpdateOverlay.h"

namespace kerf::ui
{

namespace
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr int numTicks = 48;

    juce::Point<float> polar (juce::Point<float> centre, float radius, float angle) // 0 = up, clockwise
    {
        return { centre.x + radius * std::sin (angle), centre.y - radius * std::cos (angle) };
    }

    // Faint echoes on the radar (angle, distance from the centre 0..1): they light up as the sweep passes.
    constexpr std::array<std::pair<float, float>, 6> blips { {
        { 0.7f, 0.72f }, { 1.9f, 0.38f }, { 2.6f, 0.86f }, { 3.9f, 0.55f }, { 4.7f, 0.27f }, { 5.6f, 0.80f } } };
}

UpdateOverlay::UpdateOverlay (AnimationClock& c) : clock (c)
{
    setWantsKeyboardFocus (true);
    addChildComponent (download);
    download.setPrimary (true);
    download.onClick = [this]
    {
        if (result.downloadUrl.isNotEmpty())
            juce::URL (result.downloadUrl).launchInDefaultBrowser();
    };
    addAndMakeVisible (close);
    close.onClick = [this] { hide(); };
    clock.add (this);
}

UpdateOverlay::~UpdateOverlay()
{
    clock.remove (this);
}

void UpdateOverlay::showAndCheck()
{
    checking = true;
    settle = 1.0f;
    download.setVisible (false);
    setVisible (true);
    toFront (true);
    if (isShowing()) grabKeyboardFocus(); // (not while the window is hidden)
    resized();
    repaint();

    const auto id = ++generation;
    juce::Component::SafePointer<UpdateOverlay> safe (this);
    updates::checkAsync ([safe, id] (updates::Result r)
    {
        if (auto* self = safe.getComponent(); self != nullptr && id == self->generation)
            self->showResult (r);
    });
}

void UpdateOverlay::showResult (const updates::Result& r)
{
    checking = false;
    settle = 0.0f;
    result = r;
    download.setVisible (r.status == updates::Result::Status::newer && r.downloadUrl.isNotEmpty());
    if (! isVisible())
    {
        setVisible (true);
        toFront (true);
    }
    resized();
    repaint();
}

void UpdateOverlay::hide()
{
    ++generation;
    checking = false;
    settle = 1.0f;
    setVisible (false);
}

void UpdateOverlay::animationTick (double dt)
{
    if (! isVisible())
        return;

    if (checking)
    {
        spin += dt * 0.55;
        repaint (panelBounds().toNearestInt());
    }
    else if (settle < 1.0f)
    {
        settle = std::min (1.0f, settle + (float) dt / 1.4f);
        repaint (panelBounds().toNearestInt());
    }
}

juce::Rectangle<float> UpdateOverlay::panelBounds() const
{
    return juce::Rectangle<float> (540.0f, 256.0f).withCentre (getLocalBounds().toFloat().getCentre());
}

juce::Rectangle<float> UpdateOverlay::dialBounds() const
{
    return panelBounds().reduced (24.0f, 22.0f).removeFromLeft (172.0f);
}

void UpdateOverlay::resized()
{
    const auto p = panelBounds().reduced (24.0f, 22.0f);
    close.setBounds (juce::Rectangle<float> (64.0f, 22.0f).withPosition (p.getRight() - 64.0f, p.getBottom() - 22.0f).toNearestInt());
    download.setBounds (juce::Rectangle<float> (88.0f, 22.0f).withPosition (p.getRight() - 64.0f - 8.0f - 88.0f, p.getBottom() - 22.0f).toNearestInt());
}

juce::Colour UpdateOverlay::statusColour() const
{
    using Status = updates::Result::Status;
    if (checking)
        return Colours::accent;
    switch (result.status)
    {
        case Status::upToDate:      return Colours::ok;
        case Status::newer:         return Colours::accent;
        case Status::notConfigured: return Colours::textDim;
        case Status::failed:        return Colours::record;
    }
    return Colours::textDim;
}

void UpdateOverlay::paintDial (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto centre = area.getCentre();
    const auto radius = std::min (area.getWidth(), area.getHeight()) * 0.5f - 18.0f;
    const auto colour = statusColour();
    const auto sweep = (float) (spin - std::floor (spin)) * twoPi;

    // The screen: a recessed disc with a glow of the answer's colour.
    const auto screen = juce::Rectangle<float> (radius * 2.0f + 12.0f, radius * 2.0f + 12.0f).withCentre (centre);
    g.setColour (Colours::well);
    g.fillEllipse (screen);
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.10f), centre, colour.withAlpha (0.0f), centre.translated (radius, 0.0f), true));
    g.fillEllipse (screen);
    g.setColour (Colours::hairline);
    g.drawEllipse (screen, 1.0f);

    // Range rings and the cross.
    for (const auto f : { 0.34f, 0.67f, 1.0f })
    {
        g.setColour (Colours::line.withAlpha (f == 1.0f ? 0.9f : 0.5f));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f * f, radius * 2.0f * f).withCentre (centre), 1.0f);
    }
    g.setColour (Colours::hairline);
    g.drawLine ({ polar (centre, radius, 0.0f), polar (centre, radius, twoPi * 0.5f) }, 1.0f);
    g.drawLine ({ polar (centre, radius, twoPi * 0.25f), polar (centre, radius, twoPi * 0.75f) }, 1.0f);

    // Ticks round the edge: with the answer they light up in its colour, one after another.
    const auto lit = checking ? 0.0f : juce::jlimit (0.0f, 1.0f, settle * 1.6f);
    for (int i = 0; i < numTicks; ++i)
    {
        const auto a = twoPi * (float) i / (float) numTicks;
        const auto major = i % 4 == 0;
        const auto on = (float) i / (float) numTicks < lit;
        g.setColour (on ? colour.withAlpha (major ? 0.95f : 0.6f) : Colours::textFaint.withAlpha (major ? 0.8f : 0.4f));
        g.drawLine ({ polar (centre, radius + 10.0f, a), polar (centre, radius + 10.0f + (major ? 6.0f : 3.0f), a) }, major ? 1.5f : 1.0f);
    }

    if (checking)
    {
        // The sweep: a fading wedge behind a bright line, echoes lighting up as it passes.
        constexpr int steps = 24;
        const auto trail = twoPi * 0.3f;
        const auto disc = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        for (int k = 0; k < steps; ++k)
        {
            const auto a1 = sweep - trail * (float) k / steps, a0 = sweep - trail * (float) (k + 1) / steps;
            juce::Path wedge;
            wedge.addPieSegment (disc, a0, a1, 0.0f);
            g.setColour (Colours::accent.withAlpha (0.3f * std::pow (1.0f - (float) k / steps, 1.5f)));
            g.fillPath (wedge);
        }
        g.setColour (Colours::accent);
        g.drawLine ({ centre, polar (centre, radius, sweep) }, 1.5f);

        for (const auto& [angle, distance] : blips)
        {
            auto since = sweep - angle;
            since -= twoPi * std::floor (since / twoPi);
            const auto glow = std::exp (-since * 1.4f);
            const auto p = polar (centre, radius * distance, angle);
            g.setColour (Colours::accent.withAlpha (0.15f + 0.85f * glow));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (p));
            if (glow > 0.05f)
            {
                g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.35f * glow), p, Colours::accent.withAlpha (0.0f), p.translated (9.0f, 0.0f), true));
                g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (p));
            }
        }

        const auto beat = 0.5f + 0.5f * (float) std::sin (spin * twoPi * 3.0);
        g.setColour (Colours::accent.withAlpha (0.5f + 0.5f * beat));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre));
        return;
    }

    // The answer: a pulse running out from the centre (once), and its symbol.
    for (const auto delay : { 0.0f, 0.18f })
    {
        const auto t = juce::jlimit (0.0f, 1.0f, (settle - delay) / (1.0f - delay));
        if (t <= 0.0f || t >= 1.0f)
            continue;
        const auto eased = 1.0f - (1.0f - t) * (1.0f - t);
        const auto r = 22.0f + (radius + 6.0f - 22.0f) * eased;
        g.setColour (colour.withAlpha (0.7f * (1.0f - t)));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 2.0f * (1.0f - t) + 0.75f);
    }

    const auto pop = juce::jlimit (0.0f, 1.0f, settle * 4.0f);
    const auto s = 0.7f + 0.3f * (1.0f - (1.0f - pop) * (1.0f - pop));
    const auto badge = juce::Rectangle<float> (44.0f * s, 44.0f * s).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.28f), centre, colour.withAlpha (0.0f), centre.translated (40.0f, 0.0f), true));
    g.fillEllipse (badge.expanded (18.0f));
    g.setColour (Colours::surface);
    g.fillEllipse (badge);
    g.setColour (colour.withAlpha (0.18f));
    g.fillEllipse (badge);
    g.setColour (colour);
    g.drawEllipse (badge, 1.5f);

    using Status = updates::Result::Status;
    const auto at = [centre, s] (float x, float y) { return juce::Point<float> (centre.x + x * s, centre.y + y * s); };
    juce::Path icon;
    switch (result.status)
    {
        case Status::upToDate: // a tick
            icon.startNewSubPath (at (-8.0f, 0.5f));
            icon.lineTo (at (-2.5f, 6.0f));
            icon.lineTo (at (8.5f, -6.0f));
            break;
        case Status::newer: // an arrow down onto a line
            icon.startNewSubPath (at (0.0f, -9.0f));
            icon.lineTo (at (0.0f, 5.0f));
            icon.startNewSubPath (at (-5.5f, -0.5f));
            icon.lineTo (at (0.0f, 5.0f));
            icon.lineTo (at (5.5f, -0.5f));
            icon.startNewSubPath (at (-7.5f, 9.5f));
            icon.lineTo (at (7.5f, 9.5f));
            break;
        case Status::failed: // a cross
            icon.startNewSubPath (at (-6.5f, -6.5f));
            icon.lineTo (at (6.5f, 6.5f));
            icon.startNewSubPath (at (6.5f, -6.5f));
            icon.lineTo (at (-6.5f, 6.5f));
            break;
        case Status::notConfigured: // a question mark
            icon.startNewSubPath (at (-5.5f, -4.0f));
            icon.cubicTo (at (-5.5f, -11.0f), at (5.5f, -11.0f), at (5.5f, -4.5f));
            icon.cubicTo (at (5.5f, -0.5f), at (0.0f, -0.5f), at (0.0f, 3.5f));
            g.fillEllipse (juce::Rectangle<float> (3.4f * s, 3.4f * s).withCentre (at (0.0f, 8.5f)));
            break;
    }
    g.strokePath (icon, juce::PathStrokeType (2.2f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void UpdateOverlay::paint (juce::Graphics& g)
{
    const auto panel = panelBounds();
    const auto colour = statusColour();

    g.setColour (Colours::background.withAlpha (0.86f));
    g.fillRect (getLocalBounds());
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.10f), panel.getCentre(), colour.withAlpha (0.0f),
                                             panel.getCentre().translated (panel.getWidth() * 0.85f, 0.0f), true));
    g.fillRect (getLocalBounds());

    // The panel: lit from the top edge in the answer's colour.
    g.setGradientFill (juce::ColourGradient (Colours::surfaceRaised, 0.0f, panel.getY(), Colours::surface, 0.0f, panel.getBottom(), false));
    g.fillRect (panel);
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.09f), 0.0f, panel.getY(), colour.withAlpha (0.0f), 0.0f, panel.getY() + 70.0f, false));
    g.fillRect (panel.withHeight (70.0f));
    g.setColour (Colours::line);
    g.drawRect (panel, 1.0f);
    juce::ColourGradient edge (colour, panel.getX(), 0.0f, colour.withAlpha (0.1f), panel.getRight(), 0.0f, false);
    edge.addColour (0.35, colour.withAlpha (0.8f));
    g.setGradientFill (edge);
    g.fillRect (panel.withHeight (2.0f));

    paintDial (g, dialBounds());

    // The words.
    auto r = panel.reduced (24.0f, 22.0f);
    r.removeFromLeft (172.0f + 24.0f);
    drawText (g, "CHECK FOR UPDATES", r.removeFromTop (14.0f), TextStyle::sectionLabel, Colours::textDim);
    r.removeFromTop (16.0f);

    using Status = updates::Result::Status;
    auto versions = r.removeFromTop (50.0f);
    auto installed = versions.removeFromLeft (versions.getWidth() * 0.5f);
    const auto latestKnown = ! checking && (result.status == Status::upToDate || result.status == Status::newer);

    // The number large; a pre-release word (BETA) beside the label.
    const auto stage = juce::String (KERF_VERSION_LABEL).fromFirstOccurrenceOf (KERF_VERSION, false, false).trim().toUpperCase();
    auto installedLabel = installed.removeFromTop (14.0f);
    drawText (g, "INSTALLED", installedLabel, TextStyle::label, Colours::textFaint);
    if (stage.isNotEmpty())
        drawText (g, stage, installedLabel.withTrimmedLeft (textWidth ("INSTALLED", TextStyle::label) + 8.0f), TextStyle::label, Colours::accent);
    drawText (g, "LATEST", versions.removeFromTop (14.0f), TextStyle::label, Colours::textFaint);
    g.setFont (Fonts::mono (30.0f));
    g.setColour (Colours::text);
    g.drawText (KERF_VERSION, installed.toNearestInt(), juce::Justification::centredLeft, false);

    if (checking)
    {
        // Asking: three dots in fixed places, lighting up one after another (text dots moved
        // as the line grew).
        const auto lit = 1 + (int) (spin * 6.0) % 3;
        for (int k = 0; k < 3; ++k)
        {
            g.setColour (k < lit ? Colours::textDim : Colours::textFaint.withAlpha (0.35f));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ versions.getX() + 3.0f + 12.0f * (float) k, versions.getCentreY() + 7.0f }));
        }
    }
    else
    {
        const auto latest = latestKnown ? (result.version.isNotEmpty() ? result.version : juce::String (KERF_VERSION))
                                        : juce::String::fromUTF8 ("\xe2\x80\x94"); // an em dash: nothing known
        g.setColour (latestKnown ? colour : Colours::textFaint);
        g.drawText (latest, versions.toNearestInt(), juce::Justification::centredLeft, false);
    }

    r.removeFromTop (16.0f);
    g.setColour (Colours::hairline);
    g.fillRect (r.removeFromTop (1.0f));
    r.removeFromTop (14.0f);

    juce::String headline, detail;
    if (checking)
    {
        headline = "Checking for updates" + juce::String::repeatedString (".", 1 + (int) (spin * 6.0) % 3);
        detail = "Asking the update page for the latest version.";
    }
    else switch (result.status)
    {
        case Status::upToDate:      headline = "You have the latest version."; detail = "Nothing to do. Back to the music."; break;
        case Status::newer:         headline = "KERF " + result.version + " is available."; detail = "Download it and install it over this version."; break;
        case Status::notConfigured: headline = "No version published yet."; detail = "New versions show up here as soon as they are released."; break;
        case Status::failed:        headline = "Could not check for updates."; detail = "No connection to the update page. Try again later."; break;
    }

    drawText (g, headline, r.removeFromTop (20.0f), TextStyle::bodyStrong, Colours::text);
    r.removeFromTop (2.0f);
    g.setFont (Fonts::get (TextStyle::body));
    g.setColour (Colours::textDim);
    g.drawFittedText (detail, r.removeFromTop (34.0f).toNearestInt(), juce::Justification::topLeft, 2, 1.0f);
}

void UpdateOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.position))
        hide();
}

bool UpdateOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hide();
        return true;
    }
    return false;
}

} // namespace kerf::ui
