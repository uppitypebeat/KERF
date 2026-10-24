#include "EqView.h"
#include "ScreenStyle.h"

namespace kerf::ui
{

namespace
{
    constexpr float minHz = 20.0f, maxHz = 30000.0f; // the EQ reaches 30 kHz (the grid is labelled to 20 kHz)

    juce::String hzText (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    const juce::StringArray typeNames { "BELL", "LOW SHELF", "HIGH SHELF", "LOW CUT", "HIGH CUT", "NOTCH" };


}

std::unique_ptr<juce::Drawable> eqTypeIcon (int type, juce::Colour colour)
{
    juce::Path p;
    switch (type)
    {
        case 0:  p.startNewSubPath (0.0f, 9.0f); p.lineTo (5.0f, 9.0f); p.cubicTo (8.5f, 9.0f, 9.0f, 2.0f, 12.0f, 2.0f); p.cubicTo (15.0f, 2.0f, 15.5f, 9.0f, 19.0f, 9.0f); p.lineTo (24.0f, 9.0f); break; // bell
        case 1:  p.startNewSubPath (0.0f, 3.0f); p.lineTo (6.0f, 3.0f); p.cubicTo (10.0f, 3.0f, 12.0f, 9.0f, 16.0f, 9.0f); p.lineTo (24.0f, 9.0f); break;  // low shelf
        case 2:  p.startNewSubPath (0.0f, 9.0f); p.lineTo (8.0f, 9.0f); p.cubicTo (12.0f, 9.0f, 14.0f, 3.0f, 18.0f, 3.0f); p.lineTo (24.0f, 3.0f); break; // high shelf
        case 3:  p.startNewSubPath (2.0f, 12.0f); p.cubicTo (4.5f, 5.0f, 7.0f, 4.0f, 11.0f, 4.0f); p.lineTo (24.0f, 4.0f); break;                          // low cut
        case 4:  p.startNewSubPath (0.0f, 4.0f); p.lineTo (13.0f, 4.0f); p.cubicTo (17.0f, 4.0f, 19.5f, 5.0f, 22.0f, 12.0f); break;                         // high cut
        default: p.startNewSubPath (0.0f, 4.0f); p.lineTo (9.0f, 4.0f); p.cubicTo (11.0f, 4.0f, 11.4f, 11.0f, 12.0f, 11.0f); p.cubicTo (12.6f, 11.0f, 13.0f, 4.0f, 15.0f, 4.0f); p.lineTo (24.0f, 4.0f); break; // notch
    }
    p.startNewSubPath (0.0f, 0.0f);  // the same 24 x 12 box for every shape (equal scale)
    p.startNewSubPath (24.0f, 12.0f);

    auto icon = std::make_unique<juce::DrawablePath>();
    icon->setPath (p);
    icon->setFill (juce::FillType (juce::Colours::transparentBlack));
        icon->setStrokeFill (juce::FillType (colour));
        icon->setStrokeType (juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return icon;
}

EqView::EqView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    shown = processor.eqSettings();
    for (size_t b = 0; b < presence.size(); ++b)
    {
        presence[b] = shown.bands[b].on ? 1.0f : 0.0f;
        heardAmount[b] = shown.bands[b].on && ! shown.bands[b].bypass ? 1.0f : 0.0f;
    }
    clock.add (this);
}

EqView::~EqView()
{
    clock.remove (this);
}

void EqView::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (26.0f);
    plot = r.reduced (40.0f, 10.0f).withTrimmedBottom (12.0f);
}

juce::RangedAudioParameter* EqView::param (int band, const char* field) const
{
    return processor.getParameters().getParameter (Params::eqParamID (band, field));
}

float EqView::freqToX (float hz) const noexcept
{
    return plot.getX() + plot.getWidth() * std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
}

float EqView::xToFreq (float x) const noexcept
{
    return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth()));
}

float EqView::dbToY (float db) const noexcept
{
    return plot.getCentreY() - juce::jlimit (-maxDb, maxDb, db) / maxDb * plot.getHeight() * 0.5f;
}

float EqView::yToDb (float y) const noexcept
{
    return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * maxDb;
}

EqProcessor::Band EqView::suggestionAt (juce::Point<float> p) const
{
    // Where the mouse is says what kind of point it wants: the low end shelves (cuts at the
    // very bottom left), the high end likewise, a deep dip in the middle notches, anything
    // else is a bell.
    using Type = EqProcessor::Type;
    EqProcessor::Band b;
    b.on = true;
    b.freq = xToFreq (p.x);
    b.gainDb = juce::jlimit (-maxDb, maxDb, std::round (yToDb (p.y) * 2.0f) / 2.0f);
    if (b.freq < 45.0f && b.gainDb < -3.0f)         b.type = Type::lowCut;
    else if (b.freq > 11000.0f && b.gainDb < -3.0f) b.type = Type::highCut;
    else if (b.freq < 160.0f)                        b.type = Type::lowShelf;
    else if (b.freq > 6000.0f)                       b.type = Type::highShelf;
    else if (b.gainDb < -13.0f)                      b.type = Type::notch;
    else                                             b.type = Type::bell;
    if (! b.usesGain())
        b.gainDb = 0.0f;
    b.q = b.type == Type::lowCut || b.type == Type::highCut ? 0.707f : 0.9f; // as addEqPoint sets it
    return b;
}

void EqView::showSuggestion (juce::Point<float> p)
{
    hoverPoint = p;
    suggesting = bandAt (p) < 0 && plot.contains (p);
    repaint();
}

juce::Point<float> EqView::handlePosition (int band) const
{
    const auto& b = shown.bands[(size_t) band];
    // Cuts and notches have no gain: their handle sits on the 0 dB line.
    return { freqToX (b.freq), dbToY (b.usesGain() ? b.gainDb : 0.0f) };
}

int EqView::bandAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDist = 14.0f;
    for (int b = 0; b < EqProcessor::maxBands; ++b)
        if (shown.bands[(size_t) b].on && presence[(size_t) b] > 0.5f)
            if (const auto d = p.getDistanceFrom (handlePosition (b)); d < bestDist)
            {
                bestDist = d;
                best = b;
            }
    return best;
}

int EqView::selectedBand() const
{
    const auto b = (int) processor.getSettings().getProperty ("eqSelected", -1);
    return juce::isPositiveAndBelow (b, EqProcessor::maxBands) && shown.bands[(size_t) b].on ? b : -1;
}

void EqView::select (int band)
{
    processor.getSettings().setProperty ("eqSelected", band, nullptr);
}

void EqView::animationTick (double dt)
{
    const ScreenScope screenColours; // (colours picked here are the screen's too)
    if (! isVisible())
        return;

    if (intro < 1.0f)
        intro = std::min (1.0f, intro + (float) dt / 0.7f);

    // The curve glides to new settings (and follows automation smoothly); points fade.
    const auto target = processor.eqSettings();
    const auto k = std::min (1.0f, (float) dt * 14.0f);
    bool moving = false;
    for (size_t b = 0; b < shown.bands.size(); ++b)
    {
        auto& s = shown.bands[b];
        const auto& t = target.bands[b];
        moving = moving || std::abs (t.gainDb - s.gainDb) > 0.01f || std::abs (t.freq - s.freq) > 0.01f * t.freq
                        || std::abs (t.q - s.q) > 0.001f || std::abs ((t.on ? 1.0f : 0.0f) - presence[b]) > 0.005f;
        if (t.on && ! s.on)
            s = t; // a new point starts where it was put (it fades in)
        s.type = t.type;
        s.bypass = t.bypass;
        const auto heard = t.on && ! t.bypass ? 1.0f : 0.0f;
        moving = moving || std::abs (heard - heardAmount[b]) > 0.005f;
        heardAmount[b] += (heard - heardAmount[b]) * std::min (1.0f, (float) dt * 12.0f);
        s.gainDb += (t.gainDb - s.gainDb) * k;
        s.freq = std::exp (std::log (s.freq) + (std::log (t.freq) - std::log (s.freq)) * k);
        s.q = std::exp (std::log (s.q) + (std::log (t.q) - std::log (s.q)) * k);
        presence[b] += ((t.on ? 1.0f : 0.0f) - presence[b]) * std::min (1.0f, (float) dt * 12.0f);
        s.on = t.on || presence[b] > 0.01f;
    }

    // The spectrum behind the curve moves while there is sound; otherwise rest once the
    // points have settled.
    if (idle.update (dt, moving || intro < 1.0f || processor.isSounding(), processor.getParameterChangeCount()))
        repaint();
}

void EqView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat(), Backdrop::deep);
    const auto whiteChassis = lightTheme();

    // Header
    {
        auto h = header.reduced (12.0f, 0.0f);
        drawText (g, "EQ", h.removeFromLeft (40.0f), TextStyle::sectionLabel, Colours::text);
        drawText (g, juce::String::fromUTF8 ("Double-click: add the point shown / remove one  \xc2\xb7  Drag: move  \xc2\xb7  Wheel: width  \xc2\xb7  Right-click a point: bypass"),
                  h, TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }

    // The graph: a dark display in every theme (in white mode framed like the other screens).
    const ScreenScope screenColours;
    if (whiteChassis)
        screen::paintScreen (g, juce::Rectangle<float> (plot.getX() - 40.0f, plot.getY() - 10.0f, plot.getWidth() + 80.0f, plot.getHeight() + 32.0f), {}, 0.0f, this);

    // Grid
    // The log scale as on a real analyser: faint lines at every step of each decade (30, 40, 60 ...
    // 9k), labelled ones at 20, 50, 100, 200, 500, 1k, 2k, 5k, 10k, 20k (the EQ itself goes on to 30k).
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int k = 1; k <= 9; ++k)
        {
            const auto hz = decade * (float) k;
            if (hz <= minHz || hz >= maxHz || k == 1 || k == 2 || k == 5)
                continue; // (the labelled ones are drawn below)
            drawHairline (g, { freqToX (hz), plot.getY(), 1.0f, plot.getHeight() }, Colours::hairline.withAlpha (0.3f));
        }
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const auto x = freqToX (hz);
        drawHairline (g, { std::min (x, plot.getRight() - 1.0f), plot.getY(), 1.0f, plot.getHeight() }, Colours::hairline.withAlpha (0.7f));
        const auto text = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz);
        drawText (g, text, { x + 3.0f, plot.getBottom() + 1.0f, 40.0f, 11.0f }, TextStyle::micro, Colours::textFaint);
    }
    for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
    {
        const auto y = dbToY (db);
        drawHairline (g, { plot.getX(), y, plot.getWidth(), 1.0f }, Colours::hairline.withAlpha (0.6f));
        drawText (g, (db > 0 ? "+" : "") + juce::String ((int) db), { plot.getX() - 34.0f, y - 6.0f, 28.0f, 12.0f },
                  TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }
    drawHairline (g, { plot.getX(), plot.getCentreY(), plot.getWidth(), 1.0f }, Colours::line);

    // Live output spectrum (shaped by the EQ) as a quiet backdrop: the fine, power-averaged
    // bands, lightly blurred across frequency and drawn as one smooth curve.
    {
        constexpr int n = SpectrumAnalyzer::numFineBands;
        const auto& fine = processor.getSpectrum().getFineBands();
        std::array<juce::Point<float>, n> pts;
        for (int b = 0; b < n; ++b)
        {
            const auto at = [&fine] (int i) { return fine[(size_t) juce::jlimit (0, n - 1, i)]; };
            const auto db = (at (b - 2) + 4.0f * at (b - 1) + 6.0f * at (b) + 4.0f * at (b + 1) + at (b + 2)) / 16.0f;
            pts[(size_t) b] = { freqToX (SpectrumAnalyzer::fineBandFrequency (b)),
                                juce::jmap (juce::jlimit (-72.0f, 0.0f, db), -72.0f, 0.0f, plot.getBottom(), plot.getY() + 4.0f) };
        }

        // Quadratic segments through the midpoints: no corners anywhere.
        juce::Path line;
        line.startNewSubPath (pts[0]);
        for (int b = 1; b < n - 1; ++b)
        {
            const auto mid = (pts[(size_t) b] + pts[(size_t) b + 1]) * 0.5f;
            line.quadraticTo (pts[(size_t) b], mid);
        }
        line.lineTo (pts[(size_t) n - 1]);
        line.lineTo (plot.getRight(), pts[(size_t) n - 1].y); // (the analysis ends near 20 kHz: on, flat, to the EQ's 30k)

        auto area = line;
        area.lineTo (plot.getRight(), plot.getBottom());
        area.lineTo (pts[0].x, plot.getBottom());
        area.closeSubPath();

        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setGradientFill (juce::ColourGradient (Colours::text.withAlpha (0.11f), 0.0f, plot.getY(),
                                                 Colours::text.withAlpha (0.015f), 0.0f, plot.getBottom(), false));
        g.fillPath (area);
        g.setColour (Colours::text.withAlpha (0.2f));
        g.strokePath (line, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Response curve: every point weighted by its presence (fade in / out), and the whole
    // curve rising out of the 0 dB line when the view opens.
    const auto rise = Motion::easeOut (intro);
    // At the plugin's sample rate; above its Nyquist frequency (up to 30 kHz is shown) the curve holds.
    const auto curveRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    auto heardAt = [curveRate] (float f) { return std::min ((double) f, curveRate * 0.49); };
    std::array<EqProcessor::Biquad, EqProcessor::maxBands> filters;
    for (size_t b = 0; b < filters.size(); ++b)
        filters[b] = EqProcessor::Biquad::forBand (shown.bands[b], curveRate);

    juce::Path curve, fill;
    const auto steps = (int) plot.getWidth() / 2;
    for (int i = 0; i <= steps; ++i)
    {
        const auto x = plot.getX() + plot.getWidth() * (float) i / (float) steps;
        const auto f = xToFreq (x);
        double db = 0.0;
        for (size_t b = 0; b < filters.size(); ++b)
            if (shown.bands[b].on)
                db += presence[b] * heardAmount[b] * filters[b].magnitudeDb (heardAt (f), curveRate);
        const auto y = dbToY ((float) db * rise);
        if (i == 0) { curve.startNewSubPath (x, y); fill.startNewSubPath (x, plot.getCentreY()); fill.lineTo (x, y); }
        else        { curve.lineTo (x, y); fill.lineTo (x, y); }
    }
    fill.lineTo (plot.getRight(), plot.getCentreY());
    fill.closeSubPath();

    g.setColour (Colours::accent.withAlpha (0.12f));
    g.fillPath (fill);
    g.setColour (Colours::accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Preview of the point a double-click would add here: the curve with it (dashed), a ghost
    // handle and its type, frequency and gain next to the mouse.
    if (suggesting && dragging < 0 && hovered < 0)
    {
        const auto free = std::any_of (shown.bands.begin(), shown.bands.end(), [] (const auto& band) { return ! band.on; });
        if (free)
        {
            const auto s = suggestionAt (hoverPoint);
            const auto ghostFilter = EqProcessor::Biquad::forBand (s, curveRate);
            juce::Path ghost;
            for (int i = 0; i <= steps; ++i)
            {
                const auto x = plot.getX() + plot.getWidth() * (float) i / (float) steps;
                const auto f = xToFreq (x);
                double db = ghostFilter.magnitudeDb (heardAt (f), curveRate);
                for (size_t b = 0; b < filters.size(); ++b)
                    if (shown.bands[b].on)
                        db += presence[b] * heardAmount[b] * filters[b].magnitudeDb (heardAt (f), curveRate);
                const auto y = dbToY ((float) db);
                if (i == 0) ghost.startNewSubPath (x, y); else ghost.lineTo (x, y);
            }
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createDashedStroke (dashed, ghost, dashes, 2);
            g.setColour (Colours::accent.withAlpha (0.6f));
            g.fillPath (dashed);

            const auto handle = juce::Point<float> (freqToX (s.freq), dbToY (s.usesGain() ? s.gainDb : 0.0f));
            const auto ring = juce::Rectangle<float> (14.0f, 14.0f).withCentre (handle);
            g.setColour (Colours::accent.withAlpha (0.15f));
            g.fillEllipse (ring);
            juce::Path circle;
            circle.addEllipse (ring);
            juce::Path dashedCircle;
            const float dots[] = { 2.5f, 2.5f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashedCircle, circle, dots, 2);
            g.setColour (Colours::accent);
            g.fillPath (dashedCircle);

            auto text = typeNames[(int) s.type] + "   " + hzText (s.freq);
            if (s.usesGain())
                text << "   " << (s.gainDb > 0.05f ? "+" : "") << juce::String (s.gainDb, 1) << " dB";
            const auto w = textWidth (text, TextStyle::micro) + 46.0f;
            auto box = juce::Rectangle<float> (w, 20.0f).withPosition (hoverPoint.x + 14.0f, hoverPoint.y - 30.0f);
            if (box.getRight() > plot.getRight()) box.setX (hoverPoint.x - 14.0f - w);
            if (box.getY() < plot.getY()) box.setY (hoverPoint.y + 12.0f);
            // Like a button that is on (it is the point a double-click adds): the screen behind it, lit.
            g.setColour (backdropColour (*this, box.getCentre(), Backdrop::deep));
            g.fillRoundedRectangle (box, Face::radius);
            Face::State lit;
            lit.on = true;
            Face::key (g, box, lit);
            if (const auto icon = eqTypeIcon ((int) s.type, Colours::accent))
                icon->drawWithin (g, box.withTrimmedLeft (8.0f).withWidth (24.0f).reduced (0.0f, 4.0f), juce::RectanglePlacement::centred, 1.0f);
            drawText (g, text, box.withTrimmedLeft (38.0f), TextStyle::micro, Colours::text);
        }
    }

    // Points (numbered), popping in one after another when the view opens.
    const auto selected = selectedBand();
    for (int b = 0; b < EqProcessor::maxBands; ++b)
    {
        if (! shown.bands[(size_t) b].on)
            continue;

        const auto pop = Motion::easeOut (juce::jlimit (0.0f, 1.0f, intro * 2.0f - 0.08f * (float) b));
        const auto alpha = presence[(size_t) b] * pop;
        auto pos = handlePosition (b);
        pos.y = plot.getCentreY() + (pos.y - plot.getCentreY()) * rise;
        const auto hot = b == hovered || b == dragging;
        const auto isSel = b == selected;
        const auto r = (hot || isSel ? 8.5f : 7.0f) * (0.4f + 0.6f * alpha);
        const auto circle = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos);

        if (shown.bands[(size_t) b].bypass)
        {
            // Bypassed: there, but not heard - hollow, faint, struck through.
            const auto faint = alpha * (hot || isSel ? 0.85f : 0.55f);
            g.setColour (Colours::surface.withMultipliedAlpha (alpha));
            g.fillEllipse (circle);
            g.setColour (Colours::textDim.withMultipliedAlpha (faint));
            g.drawEllipse (circle, isSel ? 2.0f : 1.2f);
            g.drawLine ({ circle.getBottomLeft().translated (r * 0.3f, -r * 0.3f), circle.getTopRight().translated (-r * 0.3f, r * 0.3f) }, 1.2f);
            drawText (g, juce::String (b + 1), circle, TextStyle::micro, Colours::textDim.withMultipliedAlpha (faint), juce::Justification::centred);
            continue;
        }
        g.setColour ((isSel ? Colours::accent : Colours::surface).withMultipliedAlpha (alpha));
        g.fillEllipse (circle);
        g.setColour (Colours::accent.withMultipliedAlpha (alpha));
        g.drawEllipse (circle, hot ? 2.0f : 1.5f);
        drawText (g, juce::String (b + 1), circle, TextStyle::micro,
                  (isSel ? Colours::background : Colours::text).withMultipliedAlpha (alpha), juce::Justification::centred);
    }

    // Readout for the point under the mouse / being dragged / selected
    const auto b = dragging >= 0 ? dragging : (hovered >= 0 ? hovered : selected);
    if (b >= 0 && shown.bands[(size_t) b].on)
    {
        const auto& band = shown.bands[(size_t) b];
        auto text = "EQ " + juce::String (b + 1) + "   " + typeNames[(int) band.type] + "   " + hzText (band.freq);
        if (band.usesGain())
            text << "   " << (band.gainDb > 0.05f ? "+" : "") << juce::String (band.gainDb, 1) << " dB";
        if (band.type != EqProcessor::Type::lowShelf && band.type != EqProcessor::Type::highShelf)
            text << "   Q " << juce::String (band.q, 2);
        if (band.bypass)
            text << "   BYPASSED";
        // (A size up from the other annotations: it is what you read while dragging.)
        const auto w = textWidth (text, TextStyle::value) + 20.0f;
        const auto box = juce::Rectangle<float> (w, 22.0f).withPosition (plot.getRight() - w, plot.getY() + 4.0f);
        // Like the buttons: the screen behind it (the curve does not show through), a fine outline.
        g.setColour (backdropColour (*this, box.getCentre(), Backdrop::deep));
        g.fillRoundedRectangle (box, Face::radius);
        Face::key (g, box, {});
        drawText (g, text, box, TextStyle::value, Colours::text, juce::Justification::centred);
    }
    else if (shown.isFlat() && std::all_of (presence.begin(), presence.end(), [] (float p) { return p < 0.01f; }))
    {
        // Pulses softly until the first point is added.
        const auto resting = idle.quiet >= IdleGate::settleSeconds * 0.85; // the last frames before rest: fully lit
        const auto pulse = resting ? 1.0f : 0.5f + 0.5f * (float) std::sin (juce::Time::getMillisecondCounterHiRes() * 0.001 * juce::MathConstants<double>::twoPi / 1.6);
        drawText (g, "DOUBLE-CLICK TO ADD A POINT", plot.withTrimmedTop (plot.getHeight() * 0.5f + 10.0f).withHeight (16.0f),
                  TextStyle::label, Colours::textDim.interpolatedWith (Colours::accent, 0.5f * pulse).withMultipliedAlpha (0.3f + 0.7f * pulse),
                  juce::Justification::centred);
    }
}

void EqView::mouseMove (const juce::MouseEvent& e)
{
    idle.wake();
    const auto b = bandAt (e.position);
    setMouseCursor (b >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
    hovered = b;
    showSuggestion (e.position);
}

void EqView::mouseExit (const juce::MouseEvent&)
{
    idle.wake();
    hovered = -1;
    suggesting = false;
}

void EqView::mouseDown (const juce::MouseEvent& e)
{
    idle.wake();
    const auto b = bandAt (e.position);

    if (e.mods.isPopupMenu())
    {
        // Right-click on a point: bypass it (it stays, but is not heard), again: on.
        if (b >= 0)
        {
            select (b);
            processor.toggleEqBypass (b);
        }
        return;
    }

    dragging = b;
    if (b < 0)
        return;

    select (b);
    param (b, Params::EqField::freq)->beginChangeGesture();
    if (shown.bands[(size_t) b].usesGain())
        param (b, Params::EqField::gain)->beginChangeGesture();
}

void EqView::mouseDrag (const juce::MouseEvent& e)
{
    idle.wake();
    if (dragging < 0)
        return;

    auto* freqParam = param (dragging, Params::EqField::freq);
    freqParam->setValueNotifyingHost (freqParam->convertTo0to1 (xToFreq (e.position.x))); // the range clamps it
    if (shown.bands[(size_t) dragging].usesGain())
    {
        auto* gainParam = param (dragging, Params::EqField::gain);
        gainParam->setValueNotifyingHost (gainParam->convertTo0to1 (juce::jlimit (-18.0f, 18.0f, yToDb (e.position.y))));
    }
}

void EqView::mouseUp (const juce::MouseEvent&)
{
    if (dragging >= 0)
    {
        param (dragging, Params::EqField::freq)->endChangeGesture();
        if (shown.bands[(size_t) dragging].usesGain())
            param (dragging, Params::EqField::gain)->endChangeGesture();
    }
    dragging = -1;
}

void EqView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto b = bandAt (e.position); b >= 0)
    {
        processor.removeEqPoint (b);
        hovered = -1;
        return;
    }

    if (! plot.expanded (0.0f, 8.0f).contains (e.position))
        return;

    const auto s = suggestionAt (e.position); // the point the preview showed
    const auto band = processor.addEqPoint (s.freq, s.gainDb, s.type);
    if (band >= 0)
        select (band);
    else
        processor.setStatusMessage ("All " + juce::String (Params::eqBands) + " EQ points are in use");
}

void EqView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    auto b = bandAt (e.position);
    if (b < 0)
        b = selectedBand();
    if (b < 0)
        return;

    auto* q = param (b, Params::EqField::q);
    q->beginChangeGesture();
    q->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, q->getValue() + wheel.deltaY * 0.15f));
    q->endChangeGesture();
}

} // namespace kerf::ui

namespace kerf::ui
{

EqTypeTools::TypeButton::TypeButton (int t) : juce::Button (typeNames[t]), type (t)
{
    setWantsKeyboardFocus (false); // Space stays with the editor
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void EqTypeTools::TypeButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    // Like the other tool symbols: a small raised box, lit in the accent colour when on.
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();
    Face::State face;
    face.on = on;
    face.hover = highlighted;
    face.down = down;
    face.enabled = isEnabled();
    Face::key (g, b, face);
    const auto colour = Face::text (face);
    if (const auto icon = eqTypeIcon (type, colour))
        icon->drawWithin (g, b.reduced (5.0f, 5.0f), juce::RectanglePlacement::centred, isEnabled() ? 1.0f : 0.4f);
}

EqTypeTools::EqTypeTools (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    static const char* tips[] = {
        "BELL: boost or cut around a frequency",
        "LOW SHELF: boost or cut the lows",
        "HIGH SHELF: boost or cut the highs",
        "LOW CUT: remove the lows",
        "HIGH CUT: remove the highs",
        "NOTCH: remove a narrow band"
    };
    for (int t = 0; t < numTypes; ++t)
    {
        auto button = std::make_unique<TypeButton> (t);
        button->setTooltip (tips[t]);
        button->onClick = [this, t] { choose (t); };
        addAndMakeVisible (*button);
        buttons.push_back (std::move (button));
    }
    clock.add (this);
    animationTick (0.0);
}

EqTypeTools::~EqTypeTools()
{
    clock.remove (this);
}

void EqTypeTools::resized()
{
    auto r = getLocalBounds();
    for (auto& b : buttons)
    {
        b->setBounds (r.removeFromLeft (buttonWidth).withSizeKeepingCentre (buttonWidth, 22));
        r.removeFromLeft (gap);
    }
}

int EqTypeTools::selectedBand() const
{
    const auto band = (int) processor.getSettings().getProperty ("eqSelected", -1);
    if (! juce::isPositiveAndBelow (band, Params::eqBands))
        return -1;
    return processor.eqSettings().bands[(size_t) band].on ? band : -1;
}

void EqTypeTools::choose (int type)
{
    if (const auto band = selectedBand(); band >= 0)
    {
        processor.setParameterAsGesture (Params::eqParamID (band, Params::EqField::type), (float) type);
        return;
    }
    // No point selected: a new one of this type, where that type usually goes.
    const auto t = (EqProcessor::Type) type;
    const auto band = t == EqProcessor::Type::lowCut    ? processor.addEqPoint (40.0f, 0.0f, t)
                    : t == EqProcessor::Type::highCut   ? processor.addEqPoint (16000.0f, 0.0f, t)
                    : t == EqProcessor::Type::lowShelf  ? processor.addEqPoint (120.0f, 0.0f, t)
                    : t == EqProcessor::Type::highShelf ? processor.addEqPoint (8000.0f, 0.0f, t)
                                                        : processor.addEqPointInGap (t);
    if (band >= 0)
        processor.getSettings().setProperty ("eqSelected", band, nullptr);
    else
        processor.setStatusMessage ("All " + juce::String (Params::eqBands) + " EQ points are in use");
}

void EqTypeTools::animationTick (double)
{
    if (! isVisible())
        return;
    // The selected point's type is lit.
    const auto band = selectedBand();
    const auto current = band >= 0 ? (int) processor.eqSettings().bands[(size_t) band].type : -1;
    for (auto& b : buttons)
        if (b->getToggleState() != (b->type == current))
            b->setToggleState (b->type == current, juce::dontSendNotification);
}

} // namespace kerf::ui
