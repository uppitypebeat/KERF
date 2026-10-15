#pragma once

#include "Theme.h"

/** The "synth display" look shared by GRANULIZER and FX: a glowing screen with corner ticks,
    HUD lettering, glowing strokes and CRT scanlines. Dark in the night themes; in white mode
    the screen is light (the theme's sky shows through, the HUD is dark ink). */
namespace kerf::ui::screen
{
    inline juce::Colour bg()   { return lightTheme() ? Colours::well : juce::Colour (0xff0c0c0e); }
    inline juce::Colour edge() { return lightTheme() ? Colours::line : juce::Colour (0xff2a2a2e); }
    inline juce::Colour hud()  { return lightTheme() ? Colours::text : juce::Colour (0xffd9d4ca); }

    inline juce::Colour signal() { return Colours::accent; }

    /** A hot core of a colour for cores and peaks: white-hot at night, deeper and more intense
        in white mode (white would vanish there). */
    inline juce::Colour heat (juce::Colour c, float amount)
    {
        amount = juce::jlimit (0.0f, 1.0f, amount);
        return lightTheme() ? c.interpolatedWith (c.darker (0.9f), amount * 0.7f) : c.interpolatedWith (juce::Colours::white, amount);
    }
    inline juce::Colour hot (float amount) { return heat (signal(), amount); }

    inline void hudText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float alpha = 0.55f,
                         juce::Justification just = juce::Justification::centredLeft)
    {
        g.setColour (hud().withAlpha (alpha));
        g.setFont (Fonts::mono (9.5f, true).withExtraKerningFactor (0.08f));
        g.drawText (text, area, just, false);
    }

    inline void glowStroke (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width, float glowAmount)
    {
        if (glowAmount > 0.0f)
        {
            // The halos: bevelled joints (soft and wide, they look the same, and cost far less
            // than round ones on a long path).
            g.setColour (c.withMultipliedAlpha (0.10f * glowAmount));
            g.strokePath (p, juce::PathStrokeType (width * 6.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
            g.setColour (c.withMultipliedAlpha (0.22f * glowAmount));
            g.strokePath (p, juce::PathStrokeType (width * 2.6f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    /** Screen background, edge, corner ticks and a label in the top-left corner. With the
        `owner` component given, the Nebula theme shows its sky through the screen. */
    inline void paintScreen (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label, float labelAlpha = 0.6f,
                             const juce::Component* owner = nullptr)
    {
        if (owner != nullptr && nebulaTheme())
        {
            fillBackdrop (g, *owner, area, Backdrop::deep, 3.0f);
        }
        else
        {
            g.setColour (bg());
            g.fillRoundedRectangle (area, 3.0f);
        }
        g.setColour (edge());
        g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);

        g.setColour (hud().withAlpha (0.18f));
        const auto t = 7.0f;
        const auto r = area.reduced (5.0f);
        for (auto c : { r.getTopLeft(), r.getTopRight(), r.getBottomLeft(), r.getBottomRight() })
        {
            const auto sx = c.x < r.getCentreX() ? 1.0f : -1.0f, sy = c.y < r.getCentreY() ? 1.0f : -1.0f;
            g.fillRect (juce::Rectangle<float> (std::min (c.x, c.x + sx * t), c.y - (sy < 0 ? 1.0f : 0.0f), t, 1.0f));
            g.fillRect (juce::Rectangle<float> (c.x - (sx < 0 ? 1.0f : 0.0f), std::min (c.y, c.y + sy * t), 1.0f, t));
        }

        hudText (g, label, area.reduced (12.0f, 8.0f).withHeight (12.0f), labelAlpha);
    }

    inline void scanlines (juce::Graphics& g, juce::Rectangle<float> area, float alpha = 0.10f)
    {
        // On whole pixels: fractional rows are anti-aliased edge by edge, which cost more than
        // everything else on a screen.
        g.setColour (juce::Colours::black.withAlpha (lightTheme() ? alpha * 0.35f : alpha));
        const auto r = area.getSmallestIntegerContainer();
        for (auto y = r.getY(); y < r.getBottom(); y += 3)
            g.fillRect (r.getX(), y, r.getWidth(), 1);
    }

    /** Opening animation: the screen switches on like a CRT (a bright line opening into the
        picture). `t` 0..1. */
    inline void powerOn (juce::Graphics& g, juce::Rectangle<float> area, float t)
    {
        if (t >= 1.0f)
            return;
        const auto s = area.reduced (1.0f);
        const auto open = Motion::easeOut (juce::jlimit (0.0f, 1.0f, t));
        const auto band = s.withSizeKeepingCentre (s.getWidth(), std::max (1.5f, s.getHeight() * open));
        g.setColour (bg());
        g.fillRect (s.withBottom (band.getY()));
        g.fillRect (s.withTop (band.getBottom()));
        g.setColour (signal().withAlpha ((1.0f - open) * 0.9f + 0.1f * (1.0f - t)));
        g.fillRect (juce::Rectangle<float> (s.getX(), band.getY() - 0.5f, s.getWidth(), 1.5f));
        g.fillRect (juce::Rectangle<float> (s.getX(), band.getBottom() - 1.0f, s.getWidth(), 1.5f));
    }
}
