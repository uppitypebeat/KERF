#include "GuideOverlay.h"

namespace kerf::ui
{

namespace
{
    constexpr float cardWidth = 340.0f, cardHeight = 136.0f;
    const char* const seenKey = "guideSeen";
}

GuideOverlay::GuideOverlay (AnimationClock& c) : clock (c)
{
    setWantsKeyboardFocus (true);
    for (auto* b : { &next, &back, &skip })
        addAndMakeVisible (b);

    next.onClick = [this]
    {
        if (step + 1 < (int) steps.size())
            showStep (step + 1);
        else
        {
            finish();
            if (onStarted) onStarted();
        }
    };
    back.onClick = [this] { if (step > 0) showStep (step - 1); };
    skip.onClick = [this] { finish(); };
    clock.add (this);
}

GuideOverlay::~GuideOverlay()
{
    clock.remove (this);
}

bool GuideOverlay::shouldShowOnStart()
{
    if (juce::SystemStats::getEnvironmentVariable ("KERF_NO_GUIDE", {}).isNotEmpty())
        return false;
    return loadUserSetting (seenKey, "0") != "1";
}

void GuideOverlay::setSteps (std::vector<Step> newSteps)
{
    steps = std::move (newSteps);
}

void GuideOverlay::start()
{
    if (steps.empty())
        return;
    setVisible (true);
    toFront (true);
    shownSpot = {};
    showStep (0);
    if (isShowing()) grabKeyboardFocus(); // (not while the window is hidden)
}

void GuideOverlay::finish()
{
    setVisible (false);
    if (juce::SystemStats::getEnvironmentVariable ("KERF_NO_GUIDE", {}).isEmpty())
        saveUserSetting (seenKey, "1");
}

void GuideOverlay::showStep (int index)
{
    step = juce::jlimit (0, (int) steps.size() - 1, index);
    if (steps[(size_t) step].onShow)
        steps[(size_t) step].onShow();
    fade = 0.0f;
    next.setButtonText (step + 1 < (int) steps.size() ? "Next" : "Start");
    back.setEnabled (step > 0);
    if (shownSpot.isEmpty())
        shownSpot = spotlight();
    resized();
    repaint();
}

juce::Rectangle<float> GuideOverlay::spotlight() const
{
    if (steps.empty() || steps[(size_t) step].target == nullptr)
        return getLocalBounds().toFloat().withSizeKeepingCentre (10.0f, 10.0f);
    return steps[(size_t) step].target().toFloat().expanded (6.0f).getIntersection (getLocalBounds().toFloat().reduced (2.0f));
}

juce::Rectangle<float> GuideOverlay::cardBounds() const
{
    // Beside the spotlight where there is room: below, above, then to the side.
    const auto area = getLocalBounds().toFloat().reduced (16.0f);
    const auto spot = spotlight();
    juce::Rectangle<float> card (cardWidth, cardHeight);
    const auto x = juce::jlimit (area.getX(), area.getRight() - cardWidth, spot.getCentreX() - cardWidth * 0.5f);
    if (spot.getBottom() + 14.0f + cardHeight <= area.getBottom())
        return card.withPosition (x, spot.getBottom() + 14.0f);
    if (spot.getY() - 14.0f - cardHeight >= area.getY())
        return card.withPosition (x, spot.getY() - 14.0f - cardHeight);
    const auto y = juce::jlimit (area.getY(), area.getBottom() - cardHeight, spot.getCentreY() - cardHeight * 0.5f);
    if (spot.getRight() + 14.0f + cardWidth <= area.getRight())
        return card.withPosition (spot.getRight() + 14.0f, y);
    return card.withPosition (juce::jmax (area.getX(), spot.getX() - 14.0f - cardWidth), y);
}

void GuideOverlay::resized()
{
    const auto card = cardBounds().reduced (16.0f, 14.0f);
    auto row = card.withTrimmedTop (card.getHeight() - 22.0f).toNearestInt();
    next.setBounds (row.removeFromRight (64));
    row.removeFromRight (6);
    back.setBounds (row.removeFromRight (64));
    skip.setBounds (row.removeFromLeft (56));
}

void GuideOverlay::animationTick (double dt)
{
    if (! isVisible())
        return;
    time += dt;
    fade = std::min (1.0f, fade + (float) dt / 0.35f);
    const auto target = spotlight();
    const auto k = std::min (1.0f, (float) dt * 10.0f);
    auto lerp = [k] (float a, float b) { return a + (b - a) * k; };
    shownSpot = { lerp (shownSpot.getX(), target.getX()), lerp (shownSpot.getY(), target.getY()),
                  lerp (shownSpot.getWidth(), target.getWidth()), lerp (shownSpot.getHeight(), target.getHeight()) };
    repaint();
}

void GuideOverlay::paint (juce::Graphics& g)
{
    if (steps.empty())
        return;

    // The window dimmed, with a hole where the spotlight is.
    juce::Path scrim;
    scrim.setUsingNonZeroWinding (false);
    scrim.addRectangle (getLocalBounds().toFloat());
    scrim.addRoundedRectangle (shownSpot, 6.0f);
    g.setColour (juce::Colours::black.withAlpha (0.68f));
    g.fillPath (scrim);

    // A pulsing outline in the theme colour.
    const auto pulse = 0.5f + 0.5f * (float) std::sin (time * 3.0);
    g.setColour (Colours::accent.withAlpha (0.18f + 0.18f * pulse));
    g.drawRoundedRectangle (shownSpot.expanded (3.0f + 2.0f * pulse), 8.0f, 3.0f);
    g.setColour (Colours::accent);
    g.drawRoundedRectangle (shownSpot, 6.0f, 1.5f);

    // The card.
    const auto card = cardBounds();
    g.setColour (Colours::surfaceRaised);
    g.fillRoundedRectangle (card, 6.0f);
    g.setColour (Colours::accent.withAlpha (0.7f));
    g.drawRoundedRectangle (card.reduced (0.5f), 6.0f, 1.0f);

    auto inner = card.reduced (16.0f, 14.0f);
    auto top = inner.removeFromTop (16.0f);
    drawText (g, juce::String (step + 1) + " / " + juce::String ((int) steps.size()), top.removeFromRight (40.0f), TextStyle::micro,
              Colours::textFaint, juce::Justification::centredRight);
    drawText (g, steps[(size_t) step].title.toUpperCase(), top, TextStyle::sectionLabel, Colours::accent.withAlpha (fade));
    inner.removeFromTop (8.0f);
    inner.removeFromBottom (30.0f);

    g.setColour (Colours::text.withAlpha (0.4f + 0.6f * fade));
    g.setFont (Fonts::get (TextStyle::body));
    g.drawFittedText (steps[(size_t) step].text, inner.toNearestInt(), juce::Justification::topLeft, 4, 1.0f);

    // Progress dots.
    const auto dotsY = (float) skip.getBounds().getCentreY();
    const auto dotsX = (float) skip.getRight() + 10.0f;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        g.setColour ((int) i == step ? Colours::accent : Colours::textFaint.withAlpha (0.6f));
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ dotsX + 10.0f * (float) i + 5.0f, dotsY }));
    }
}

void GuideOverlay::mouseDown (const juce::MouseEvent& e)
{
    // A click in the card does nothing; elsewhere it moves on (the left button only).
    if (! e.mods.isPopupMenu() && ! cardBounds().contains (e.position))
        next.triggerClick();
}

bool GuideOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))                                         { finish(); return true; }
    if (key.isKeyCode (juce::KeyPress::rightKey) || key.isKeyCode (juce::KeyPress::returnKey)) { next.triggerClick(); return true; }
    if (key.isKeyCode (juce::KeyPress::leftKey))                                            { back.triggerClick(); return true; }
    return true; // the guide has the keyboard while it is open
}

} // namespace kerf::ui
