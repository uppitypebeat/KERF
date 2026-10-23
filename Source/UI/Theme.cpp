#include "Theme.h"
#include "BinaryData.h"

namespace kerf::ui
{

Fonts::Typefaces::Typefaces()
{
    auto load = [] (const char* data, int size) { return juce::Typeface::createSystemTypefaceFor (data, (size_t) size); };

    interRegular  = load (BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);
    interMedium   = load (BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize);
    interSemiBold = load (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize);
    monoRegular   = load (BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
    monoMedium    = load (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize);
}

const Fonts::Typefaces& Fonts::faces()
{
    // Shared, reference-counted: alive while any KERF editor (via its look-and-feel) exists,
    // released when the last one closes. Never a static that outlives JUCE at DLL unload.
    juce::SharedResourcePointer<Typefaces> shared;
    return *shared;
}

juce::Font Fonts::sans (float height, int weight)
{
    const auto& f = faces();
    auto face = weight >= 600 ? f.interSemiBold : (weight >= 500 ? f.interMedium : f.interRegular);
    return juce::Font (juce::FontOptions (face).withHeight (height));
}

juce::Font Fonts::mono (float height, bool medium)
{
    const auto& f = faces();
    return juce::Font (juce::FontOptions (medium ? f.monoMedium : f.monoRegular).withHeight (height));
}

juce::Font Fonts::get (TextStyle style)
{
    switch (style)
    {
        case TextStyle::brand:        return sans (14.0f, 600).withExtraKerningFactor (0.32f);
        case TextStyle::sectionLabel: return sans (10.0f, 600).withExtraKerningFactor (0.16f);
        case TextStyle::label:        return sans (9.5f, 500).withExtraKerningFactor (0.10f);
        case TextStyle::body:         return sans (12.0f, 400);
        case TextStyle::bodyStrong:   return sans (12.0f, 500);
        case TextStyle::value:        return mono (11.0f);
        case TextStyle::valueLarge:   return mono (15.0f);
        case TextStyle::micro:        return mono (9.5f, false);
    }
    return sans (12.0f);
}

void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
               TextStyle style, juce::Colour colour, juce::Justification just)
{
    g.setColour (colour);
    g.setFont (Fonts::get (style));
    g.drawText (text, area, just, false);
}

float textWidth (const juce::String& text, TextStyle style)
{
    return juce::GlyphArrangement::getStringWidth (Fonts::get (style), text);
}

//==============================================================================
namespace Face
{
    void key (juce::Graphics& g, juce::Rectangle<float> r, const State& s)
    {
        // Flat: no fill at rest, just a fine outline; on, a light wash of the accent.
        const auto signal = s.signal.isTransparent() ? Colours::accent : s.signal;
        const auto rad = std::min (radius, r.getHeight() * 0.3f);

        if (! s.enabled)
        {
            g.setColour (Colours::hairline);
            g.drawRoundedRectangle (r, rad, Metrics::hairline);
            return;
        }
        if (s.on)
        {
            g.setColour (signal.withAlpha (s.down ? 0.24f : (s.hover ? 0.2f : 0.15f)));
            g.fillRoundedRectangle (r, rad);
            g.setColour (signal.withAlpha (s.hover ? 0.9f : 0.6f));
            g.drawRoundedRectangle (r, rad, Metrics::hairline);
            return;
        }
        if (s.hover || s.down)
        {
            g.setColour (Colours::text.withAlpha (s.down ? 0.08f : 0.04f));
            g.fillRoundedRectangle (r, rad);
        }
        g.setColour (s.hover ? Colours::line.interpolatedWith (Colours::text, 0.25f) : Colours::line);
        g.drawRoundedRectangle (r, rad, Metrics::hairline);
    }

    void track (juce::Graphics& g, juce::Rectangle<float> r)
    {
        // Just the outline around a row of options.
        g.setColour (Colours::line);
        g.drawRoundedRectangle (r, std::min (radius, r.getHeight() * 0.3f), Metrics::hairline);
    }

    juce::Colour text (const State& s)
    {
        const auto signal = s.signal.isTransparent() ? Colours::accent : s.signal;
        if (! s.enabled) return Colours::textFaint;
        if (s.on)        return inkColour (signal, 0.35f);
        return s.hover ? Colours::text : Colours::textDim;
    }
}

//==============================================================================
//==============================================================================
const std::vector<Palette>& palettes()
{
    // Every theme is a sky: one gradient across the window, glowing in the theme colour
    // (upper left) and a second colour (lower right). The first three are night skies,
    // the last three white mode (a bright sky with stronger colour clouds, dark text).
    //                    name           background  surface     raised      hover       well        hairline    line        text        dim         faint       accent      record      ok          nebula  second glow   light
    static const std::vector<Palette> all {
        { "Nebula",      0xff0b0a10, 0xff141219, 0xff1b1922, 0xff25222d, 0xff08070c, 0xff24212b, 0xff35313e, 0xffebe6f2, 0xff8c8796, 0xff57535f, 0xff8b5cf6, 0xffe5484d, 0xff8fbf8a, true,   0xffd9509b }, // violet, magenta
        { "Aurora",      0xff090c0c, 0xff111616, 0xff181e1e, 0xff222a29, 0xff060909, 0xff222928, 0xff323b3a, 0xffe3ece8, 0xff86908c, 0xff535b58, 0xff27c49a, 0xffe5484d, 0xff8fbf8a, true,   0xff6b7cf5 }, // teal green, blue violet
        { "Ember",       0xff0b0a0d, 0xff141317, 0xff1b1a1f, 0xff25242a, 0xff08070a, 0xff252429, 0xff36353b, 0xffece7df, 0xff8e8a88, 0xff5a5856, 0xffdc4632, 0xffe5484d, 0xff8fbf8a, true,   0 },          // red, warm white
        { "Daylight",    0xfff5f1ed, 0xfffbf9f6, 0xffffffff, 0xffefeae5, 0xffebe5df, 0xffe0d9d2, 0xffcbc3bb, 0xff1d1a17, 0xff6e6862, 0xffa59e96, 0xffd9412c, 0xffd8343b, 0xff3e9a52, true,   0xff4f8ff7, true }, // coral, sky blue
        { "Glacier",     0xffeff4f5, 0xfff7fafb, 0xffffffff, 0xffe7eef0, 0xffe2eaec, 0xffd5dfe1, 0xffbfcbcf, 0xff142024, 0xff5d6c70, 0xff98a6a9, 0xff0e9e8c, 0xffd8343b, 0xff3e9a52, true,   0xff7c5cf0, true }, // teal, violet
        { "Bloom",       0xfff6f0f3, 0xfffcf8fa, 0xffffffff, 0xfff0e7ec, 0xffede2e8, 0xffe2d5dc, 0xffcfbfc8, 0xff22161d, 0xff74626b, 0xffad9ba4, 0xffd6337f, 0xffd8343b, 0xff3e9a52, true,   0xfff0a020, true }, // magenta, amber
    };
    return all;
}

namespace { int activePalette = defaultPalette(); } // Nebula (applied before the first paint)

int currentPalette() { return activePalette; }

void applyPalette (int index)
{
    index = juce::jlimit (0, (int) palettes().size() - 1, index);
    activePalette = index;
    const auto& p = palettes()[(size_t) index];

    Colours::background    = juce::Colour (p.background);
    Colours::surface       = juce::Colour (p.surface);
    Colours::surfaceRaised = juce::Colour (p.surfaceRaised);
    Colours::surfaceHover  = juce::Colour (p.surfaceHover);
    Colours::well          = juce::Colour (p.well);
    Colours::hairline      = juce::Colour (p.hairline);
    Colours::line          = juce::Colour (p.line);
    Colours::text          = juce::Colour (p.text);
    Colours::textDim       = juce::Colour (p.textDim);
    Colours::textFaint     = juce::Colour (p.textFaint);
    Colours::accent        = juce::Colour (p.accent);
    Colours::accentSoft    = juce::Colour (p.accent).withAlpha (0.2f);
    Colours::record        = juce::Colour (p.record);
    Colours::ok            = juce::Colour (p.ok);

    juce::SharedResourcePointer<ThemeHub> hub;
    hub->sendChangeMessage();
}

bool nebulaTheme()
{
    return palettes()[(size_t) activePalette].nebula;
}

namespace { bool screenColours = false; } // a ScreenScope is painting a screen

bool lightTheme()
{
    return palettes()[(size_t) activePalette].light && ! screenColours;
}

ScreenScope::ScreenScope()
{
    if (screenColours || ! palettes()[(size_t) activePalette].light)
        return;
    active = true;
    screenColours = true;
    juce::Colour* tokens[] = { &Colours::background, &Colours::surface, &Colours::surfaceRaised, &Colours::surfaceHover, &Colours::well,
                               &Colours::hairline, &Colours::line, &Colours::text, &Colours::textDim, &Colours::textFaint,
                               &Colours::accent, &Colours::accentSoft, &Colours::record, &Colours::ok };
    for (size_t i = 0; i < saved.size(); ++i)
        saved[i] = *tokens[i];

    // The dark sibling: neutral night colours with a breath of the accent, the accent a little
    // lifted (it was chosen for white).
    const auto accent = Colours::accent.brighter (0.15f);
    auto night = [accent] (juce::uint32 argb, float tint) { return juce::Colour (argb).interpolatedWith (accent, tint); };
    Colours::background    = night (0xff0b0a0d, 0.05f);
    Colours::surface       = night (0xff141317, 0.05f);
    Colours::surfaceRaised = night (0xff1b1a1f, 0.05f);
    Colours::surfaceHover  = night (0xff25242a, 0.05f);
    Colours::well          = night (0xff08070a, 0.04f);
    Colours::hairline      = night (0xff252429, 0.06f);
    Colours::line          = night (0xff36353b, 0.08f);
    Colours::text          = juce::Colour (0xffece7e4);
    Colours::textDim       = juce::Colour (0xff8e8a8c);
    Colours::textFaint     = juce::Colour (0xff5a5858);
    Colours::accent        = accent;
    Colours::accentSoft    = accent.withAlpha (0.2f);
    Colours::record        = juce::Colour (0xffe5484d);
    Colours::ok            = juce::Colour (0xff8fbf8a);
}

ScreenScope::~ScreenScope()
{
    if (! active)
        return;
    juce::Colour* tokens[] = { &Colours::background, &Colours::surface, &Colours::surfaceRaised, &Colours::surfaceHover, &Colours::well,
                               &Colours::hairline, &Colours::line, &Colours::text, &Colours::textDim, &Colours::textFaint,
                               &Colours::accent, &Colours::accentSoft, &Colours::record, &Colours::ok };
    for (size_t i = 0; i < saved.size(); ++i)
        *tokens[i] = saved[i];
    screenColours = false;
}

juce::Colour inkColour (juce::Colour c, float amount)
{
    return lightTheme() ? c.darker (amount * 1.2f) : c.brighter (amount);
}

juce::Colour nebulaGlow()
{
    const auto glow = palettes()[(size_t) activePalette].glow;
    return glow != 0 ? juce::Colour (glow) : Colours::text;
}

namespace
{
    const juce::Component* backdropRoot (const juce::Component& c)
    {
        for (auto* p = &c; p != nullptr; p = p->getParentComponent())
            if (p->getComponentID() == backdropRootId)
                return p;
        return c.getTopLevelComponent();
    }

    juce::Colour flatBackdrop (Backdrop layer) { return layer == Backdrop::panel ? Colours::background : Colours::well; }
}

void fillBackdrop (juce::Graphics& g, const juce::Component& c, juce::Rectangle<float> area, Backdrop layer, float cornerRadius)
{
    auto* lf = dynamic_cast<KerfLookAndFeel*> (&c.getLookAndFeel());
    const auto* root = nebulaTheme() && lf != nullptr ? backdropRoot (c) : nullptr;
    if (root == nullptr || root->getWidth() <= 0 || root->getHeight() <= 0)
    {
        g.setColour (flatBackdrop (layer));
        if (cornerRadius > 0.0f) g.fillRoundedRectangle (area, cornerRadius);
        else                     g.fillRect (area);
        return;
    }

    const auto& image = lf->backdropForDrawing ({ root->getWidth(), root->getHeight() }, layer);
    const auto origin = c.getLocalPoint (root, juce::Point<int>());

    juce::Graphics::ScopedSaveState save (g);
    if (cornerRadius > 0.0f)
    {
        juce::Path clip;
        clip.addRoundedRectangle (area, cornerRadius);
        g.reduceClipRegion (clip);
    }
    else
    {
        g.reduceClipRegion (area.getSmallestIntegerContainer());
    }
    g.setOpacity (1.0f); // (an image is drawn at the alpha of the colour set last: the sky is opaque)
    g.drawImageAt (image, origin.x, origin.y);
}

juce::Colour backdropColour (const juce::Component& c, juce::Point<float> p, Backdrop layer)
{
    auto* lf = dynamic_cast<KerfLookAndFeel*> (&c.getLookAndFeel());
    const auto* root = nebulaTheme() && lf != nullptr ? backdropRoot (c) : nullptr;
    if (root == nullptr || root->getWidth() <= 0 || root->getHeight() <= 0)
        return flatBackdrop (layer);

    const auto& image = lf->backdropImage ({ root->getWidth(), root->getHeight() }, layer);
    const auto at = root->getLocalPoint (&c, p).toInt();
    return image.getPixelAt (juce::jlimit (0, image.getWidth() - 1, at.x), juce::jlimit (0, image.getHeight() - 1, at.y));
}

int paletteIndex (const juce::String& name)
{
    for (size_t i = 0; i < palettes().size(); ++i)
        if (name.equalsIgnoreCase (palettes()[i].name))
            return (int) i;
    return -1;
}

int defaultPalette()
{
    return juce::jmax (0, paletteIndex ("Nebula"));
}

namespace
{
    std::unique_ptr<juce::PropertiesFile> openUserSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "KERF";
        options.folderName = "KERF";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        return std::make_unique<juce::PropertiesFile> (options);
    }
}

int loadSavedPalette()
{
    const auto index = paletteIndex (openUserSettings()->getValue ("theme", "Nebula"));
    return index >= 0 ? index : defaultPalette(); // unknown (a removed or renamed theme, e.g. "Ultraviolet"): Nebula
}

juce::String loadUserSetting (const juce::String& key, const juce::String& fallback)
{
    return openUserSettings()->getValue (key, fallback);
}

void saveUserSetting (const juce::String& key, const juce::String& value)
{
    auto settings = openUserSettings();
    settings->setValue (key, value);
    settings->saveIfNeeded();
}

void savePalette (int index)
{
    index = juce::jlimit (0, (int) palettes().size() - 1, index);
    auto settings = openUserSettings();
    settings->setValue ("theme", juce::String (palettes()[(size_t) index].name)); // stored by name: stable
    settings->saveIfNeeded();
}

//==============================================================================
KerfLookAndFeel::KerfLookAndFeel()
{
    refreshColours();
    setDefaultSansSerifTypeface (Fonts::sans (12.0f).getTypefacePtr());
}

void KerfLookAndFeel::refreshColours()
{
    setColour (juce::PopupMenu::backgroundColourId, Colours::surfaceRaised);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::surfaceHover);
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::text);
    setColour (juce::TextEditor::backgroundColourId, Colours::well);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::TextEditor::highlightColourId, Colours::accentSoft);
    setColour (juce::TextEditor::highlightedTextColourId, Colours::text);
    setColour (juce::TextEditor::outlineColourId, Colours::line);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
    setColour (juce::CaretComponent::caretColourId, Colours::accent);
    setColour (juce::TooltipWindow::backgroundColourId, Colours::surfaceRaised);
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::ScrollBar::thumbColourId, Colours::line);
}

const juce::Image& KerfLookAndFeel::backdropImage (juce::Point<int> size, Backdrop layer)
{
    auto& cache = backdrops[(layer == Backdrop::panel ? 0 : 1) + (screenColours ? 2 : 0)];
    const auto base = layer == Backdrop::panel ? Colours::background : Colours::well;
    if (cache.image.isValid() && cache.size == size && cache.base == base && cache.accent == Colours::accent && cache.glow == nebulaGlow())
        return cache.image;

    cache.size = size;
    cache.base = base;
    cache.accent = Colours::accent;
    cache.glow = nebulaGlow();

    // A wide glow in the theme colour (upper left), a fainter second one opposite and a
    // small accent wisp top right; the same field for both layers, so the screens continue
    // the sky around them. Such faint gradients band visibly in 8 bits, so every pixel gets
    // a little noise (dithering).
    const auto w = size.x, h = size.y;
    const auto fw = (float) w, fh = (float) h;
    struct Glow { juce::Point<float> centre; float radius, strength; bool accent; };
    const Glow glows[] = {
        { { 0.26f * fw, 0.30f * fh }, 0.62f * fw, 0.110f, true },
        { { 0.80f * fw, 0.78f * fh }, 0.48f * fw, palettes()[(size_t) activePalette].glow != 0 ? 0.075f : 0.050f, false }, // a colour glows less than white
        { { 0.86f * fw, 0.08f * fh }, 0.26f * fw, 0.030f, true },
    };
    // White mode: colour on white needs more of it to glow like colour on black.
    const auto layerGain = (layer == Backdrop::panel ? 1.0f : 0.85f) * (lightTheme() ? 2.1f : 1.0f);
    auto smooth = [] (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); };

    const float bg[] = { base.getFloatRed(), base.getFloatGreen(), base.getFloatBlue() };
    const float ac[] = { Colours::accent.getFloatRed(), Colours::accent.getFloatGreen(), Colours::accent.getFloatBlue() };
    const auto second = nebulaGlow();
    const float ik[] = { second.getFloatRed(), second.getFloatGreen(), second.getFloatBlue() };

    cache.image = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
    juce::Image::BitmapData data (cache.image, juce::Image::BitmapData::writeOnly);
    uint32_t seed = 0x9e3779b9u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const juce::Point<float> p ((float) x, (float) y);
            float a = 0.0f, i = 0.0f;
            for (const auto& glow : glows)
                (glow.accent ? a : i) += layerGain * glow.strength * smooth (1.0f - p.getDistanceFrom (glow.centre) / glow.radius);

            juce::uint8 rgb[3];
            for (int ch = 0; ch < 3; ++ch)
            {
                seed = seed * 1664525u + 1013904223u;
                const auto noise = (float) (seed >> 8) / (float) (1u << 24) - 0.5f;
                const auto v = (bg[ch] * (1.0f - a - i) + ac[ch] * a + ik[ch] * i) * 255.0f + noise;
                rgb[ch] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (v));
            }
            data.setPixelColour (x, y, juce::Colour (rgb[0], rgb[1], rgb[2]));
        }
    cache.native = {}; // converted on its first draw
    return cache.image;
}

const juce::Image& KerfLookAndFeel::backdropForDrawing (juce::Point<int> size, Backdrop layer)
{
    const auto& image = backdropImage (size, layer);
    auto& cache = backdrops[(layer == Backdrop::panel ? 0 : 1) + (screenColours ? 2 : 0)];
    if (! cache.native.isValid())
        cache.native = juce::NativeImageType().convert (image);
    return cache.native;
}

juce::Font KerfLookAndFeel::getPopupMenuFont() { return Fonts::get (TextStyle::body); }

void KerfLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (Colours::surface);
    g.setColour (Colours::line);
    g.drawRect (0, 0, width, height, 1);
}

void KerfLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 9;
        return;
    }

    idealHeight = 24;
    idealWidth = (int) textWidth (text, TextStyle::body) + 96; // room for tick, optional swatch and a symbol on the right
}

void KerfLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                         bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                         const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* swatch)
{
    // Minimal menu row: flat; a 2 px accent bar marks hover, a dot marks the ticked item, an
    // optional symbol sits on the right (e.g. the EQ curve shapes).
    if (isSeparator)
    {
        g.setColour (Colours::hairline);
        g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }

    auto r = area.toFloat();
    const bool hot = isHighlighted && isActive;

    if (hot)
    {
        g.setColour (Colours::surfaceHover);
        g.fillRect (r);
        g.setColour (Colours::accent);
        g.fillRect (r.withWidth (2.0f).reduced (0.0f, 4.0f));
    }

    if (isTicked)
    {
        g.setColour (Colours::accent);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ r.getX() + 13.0f, r.getCentreY() }));
    }

    auto textArea = r.withTrimmedLeft (24.0f).withTrimmedRight (22.0f);
    if (swatch != nullptr)
    {
        g.setColour (*swatch);
        g.fillRect (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ textArea.getX() + 4.5f, r.getCentreY() }));
        textArea.removeFromLeft (17.0f);
    }

    if (icon != nullptr)
    {
        const auto box = juce::Rectangle<float> (24.0f, 12.0f).withCentre ({ r.getRight() - 24.0f, r.getCentreY() });
        icon->drawWithin (g, box, juce::RectanglePlacement::centred, hot || isTicked ? 1.0f : 0.6f);
        textArea.removeFromRight (30.0f);
    }

    const auto colour = ! isActive ? Colours::textFaint : ((hot || isTicked) ? Colours::text : Colours::text.withAlpha (0.78f));
    g.setColour (colour);
    g.setFont (Fonts::sans (11.5f, isTicked ? 500 : 400));
    g.drawText (text, textArea, juce::Justification::centredLeft, true);

    if (shortcutKeyText.isNotEmpty())
        drawText (g, shortcutKeyText, r.withTrimmedRight (12.0f), TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);

    if (hasSubMenu)
    {
        // Thin chevron
        juce::Path p;
        const auto x = r.getRight() - 12.0f, y = r.getCentreY();
        p.startNewSubPath (x - 2.5f, y - 3.5f);
        p.lineTo (x + 1.0f, y);
        p.lineTo (x - 2.5f, y + 3.5f);
        g.setColour (hot ? Colours::text : Colours::textDim);
        g.strokePath (p, juce::PathStrokeType (1.2f));
    }
}

void KerfLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    drawText (g, sectionName.toUpperCase(), area.toFloat().withTrimmedLeft (12.0f).withTrimmedTop (4.0f), TextStyle::label,
              Colours::textFaint, juce::Justification::centredLeft);
}

void KerfLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
{
    g.setColour (Colours::well);
    g.fillRect (0, 0, width, height);
}

void KerfLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.hasKeyboardFocus (true) ? Colours::accent : Colours::line);
    g.drawRect (0, 0, width, height, 1);
}

juce::Rectangle<int> KerfLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto w = (int) textWidth (tipText, TextStyle::body) + 20;
    const auto h = 24;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 12,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 12,
                                 w, h).constrainedWithin (parentArea);
}

void KerfLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    g.fillAll (Colours::surfaceRaised);
    g.setColour (Colours::line);
    g.drawRect (0, 0, width, height, 1);
    drawText (g, text, { 0.0f, 0.0f, (float) width, (float) height }, TextStyle::body, Colours::text, juce::Justification::centred);
}

void KerfLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                                     int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    juce::Rectangle<int> thumb = isVertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                                            : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    g.setColour (isMouseDown ? Colours::textDim : (isMouseOver ? Colours::line.brighter (0.3f) : Colours::line));
    g.fillRect (thumb.reduced (isVertical ? 2 : 0, isVertical ? 0 : 2));
}

} // namespace kerf::ui

namespace kerf::ui
{

juce::Colour chopColour (int chopId, int firstChopId)
{
    // The first chop: the theme's own colour.
    if (chopId == firstChopId)
        return Colours::accent;

    // Hue order: terracotta, apricot, mustard, olive, pistachio, jade, teal, cerulean,
    // cornflower, periwinkle, lavender, orchid, rose-magenta, rose, tan, slate.
    static constexpr juce::uint32 palette[] = {
        0xffd9785f, 0xffdc9654, 0xffd4b45d, 0xffb2bd62, 0xff86bd7b, 0xff5fb894, 0xff52b2ae, 0xff58a8c9,
        0xff6f99d9, 0xff8a8bdc, 0xffa582d6, 0xffbd7acb, 0xffd174ab, 0xffd77289, 0xffc99a7c, 0xff98aab8
    };
    constexpr int size = (int) std::size (palette);

    // Stride 5: neighbours contrast. Skip colours that look like the accent (that one is the
    // first chop's), so no other chop is mistaken for it.
    auto index = ((chopId * 5) % size + size) % size;
    const auto accentHue = Colours::accent.getHue();
    for (int tries = 0; tries < size; ++tries)
    {
        const juce::Colour c (palette[(size_t) index]);
        auto distance = std::abs (c.getHue() - accentHue);
        distance = std::min (distance, 1.0f - distance);
        if (distance > 0.06f || c.getSaturation() < 0.2f)
            return c;
        index = (index + 1) % size;
    }
    return juce::Colour (palette[(size_t) index]);
}

} // namespace kerf::ui
