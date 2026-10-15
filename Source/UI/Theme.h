#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** KERF design system.

    Everything visual is defined here so the interface stays coherent:
      - colour tokens (one accent, used only to signal state)
      - typography scale (Inter for words, JetBrains Mono for numbers)
      - spacing (4 px grid), line weights, corner radii
      - animation timing
    Components never hard-code colours or font sizes; they ask the theme. */
namespace kerf::ui
{

/** Colour tokens of the active palette. Components read these at paint time, so
    switching the palette (applyPalette) recolours the whole UI on the next repaint. */
namespace Colours
{
    // Surfaces
    inline juce::Colour background   { 0xff111112 };
    inline juce::Colour surface      { 0xff18181a };
    inline juce::Colour surfaceRaised{ 0xff202023 };
    inline juce::Colour surfaceHover { 0xff2a2a2e };
    inline juce::Colour well         { 0xff0c0c0d }; // recessed areas: waveform, screens

    // Lines
    inline juce::Colour hairline     { 0xff2a2a2d };
    inline juce::Colour line         { 0xff3a3a3e };

    // Text
    inline juce::Colour text         { 0xffece7df };
    inline juce::Colour textDim      { 0xff8e8a84 }; // labels
    inline juce::Colour textFaint    { 0xff5a5854 }; // disabled, tertiary

    // Signal
    inline juce::Colour accent       { 0xffdc4632 }; // the one signal colour
    inline juce::Colour accentSoft   { 0x33dc4632 };
    inline juce::Colour record       { 0xffe5484d }; // recording only
    inline juce::Colour ok           { 0xff8fbf8a }; // high-confidence analysis only
}

/** A complete colour scheme: neutrals tuned around exactly one signal colour. */
struct Palette
{
    const char* name;
    juce::uint32 background, surface, surfaceRaised, surfaceHover, well, hairline, line,
                 text, textDim, textFaint, accent, record, ok;
    bool nebula = false; // backgrounds are one dark, glowing gradient across the window
    juce::uint32 glow = 0; // the gradient's second glow colour (0 = the text colour)
    bool light = false;    // a white-mode theme: a bright sky, dark text (the synth screens stay dark)
};

const std::vector<Palette>& palettes();
int currentPalette();
void applyPalette (int index);  // updates Colours:: and notifies every open editor
int paletteIndex (const juce::String& name); // -1 if unknown
int defaultPalette();           // Nebula

/** A colour per chop, stable for its id (it keeps its colour while you edit around it).
    16 muted hues around the wheel with similar brightness, so they sit calmly on the dark
    screens next to the accent; consecutive ids land far apart on the wheel, so neighbours
    always contrast. The first chop (in time) wears the theme's own colour; palette colours
    too close to it are skipped for the others. */
juce::Colour chopColour (int chopId, int firstChopId);

/** A colour as text or a thin line on the theme: lifted by `amount` on a night sky, deepened
    in white mode (where lifting would wash it out). */
juce::Colour inkColour (juce::Colour c, float amount = 0.3f);
int loadSavedPalette();         // user preference (shared by all instances and projects)
void savePalette (int index);

/** Other user preferences in the same file (e.g. whether the first-start guide was shown). */
juce::String loadUserSetting (const juce::String& key, const juce::String& fallback = {});
void saveUserSetting (const juce::String& key, const juce::String& value);

/** True for a palette whose backgrounds are a gradient (Nebula). */
bool nebulaTheme();

/** True for a white-mode palette. */
bool lightTheme();

/** The second glow colour of the gradient (lower right). */
juce::Colour nebulaGlow();

/** Backgrounds: `panel` for the chrome (header, panels, footer), `deep` for the recessed
    areas and screens. */
enum class Backdrop { panel, deep };

/** Fills `area` (in `c`'s coordinates) with the theme background. Flat palettes fill one
    colour; Nebula draws the matching piece of one gradient spanning the whole window, so
    neighbouring components join seamlessly. */
void fillBackdrop (juce::Graphics&, const juce::Component& c, juce::Rectangle<float> area, Backdrop = Backdrop::panel,
                   float cornerRadius = 0.0f);

/** The background colour at a point of `c` (for fades that must blend into it). */
juce::Colour backdropColour (const juce::Component& c, juce::Point<float> p, Backdrop = Backdrop::panel);

/** Component ID of the window-sized view the Nebula gradient is laid out on. */
inline constexpr const char* backdropRootId = "kerf.root";

/** Broadcasts palette changes to every open KERF editor. Reference-counted (no static
    JUCE objects outliving JUCE when the plugin DLL unloads). */
struct ThemeHub final : public juce::ChangeBroadcaster {};

namespace Metrics
{
    constexpr int unit = 4;            // spacing grid
    constexpr float hairline = 1.0f;
    constexpr float stroke = 1.5f;
    constexpr float strokeBold = 2.0f;
    constexpr float radiusSmall = 2.0f;

    constexpr int designWidth = 1120;  // the UI is laid out at this size and scaled
    constexpr int designHeight = 700;
}

namespace Motion
{
    constexpr double fast = 0.09;      // hover, press
    constexpr double standard = 0.16;  // state changes
    constexpr double slow = 0.32;      // view transitions
    constexpr int frameRateHz = 60;

    /** Ease-out cubic. */
    inline float easeOut (float t) noexcept { t = juce::jlimit (0.0f, 1.0f, t); return 1.0f - std::pow (1.0f - t, 3.0f); }
}

/** Typography scale. */
enum class TextStyle
{
    brand,        // KERF wordmark
    sectionLabel, // SAMPLE, PITCH ... (uppercase, tracked)
    label,        // control labels (uppercase, small, tracked)
    body,         // sample name, menu items
    bodyStrong,
    value,        // numbers (mono)
    valueLarge,   // BPM (mono)
    micro         // tiny mono annotations (chop numbers)
};

class Fonts
{
public:
    static juce::Font get (TextStyle style);
    static juce::Font mono (float height, bool medium = true);
    static juce::Font sans (float height, int weight = 400); // 400 regular, 500 medium, 600 semibold

private:
public:
    struct Typefaces
    {
        juce::Typeface::Ptr interRegular, interMedium, interSemiBold, monoRegular, monoMedium;
        Typefaces();
    };

private:
    static const Typefaces& faces();
};

/** Draws text with the theme's letterspacing for the given style. */
void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
               TextStyle style, juce::Colour colour, juce::Justification just = juce::Justification::centredLeft);

float textWidth (const juce::String& text, TextStyle style);

/** Look & feel for the few stock JUCE widgets we use (menus, text entry, tooltips). */
class KerfLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    KerfLookAndFeel();
    void refreshColours(); // after a palette change

private:
    juce::SharedResourcePointer<Fonts::Typefaces> fontsAlive; // keeps the embedded fonts loaded

public:

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
    int getPopupMenuBorderSize() override { return 3; }
    int getMenuWindowFlags() override { return 0; } // flat: no drop shadow
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    /** The Nebula gradient for a window of `size` (cached per editor; rebuilt for a new
        size or palette). */
    const juce::Image& backdropImage (juce::Point<int> size, Backdrop layer);
    /** The same, as a native image for drawing (on Windows a Direct2D bitmap: drawn without
        uploading a software image every frame). */
    const juce::Image& backdropForDrawing (juce::Point<int> size, Backdrop layer);

private:
    struct BackdropCache { juce::Image image, native; juce::Point<int> size; juce::Colour base, accent, glow; };
    BackdropCache backdrops[2];
};

} // namespace kerf::ui
