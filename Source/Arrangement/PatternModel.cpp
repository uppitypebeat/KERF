#include "PatternModel.h"

namespace kerf
{

const juce::Identifier PatternModel::PATTERN ("PATTERN"), PatternModel::EVENT ("EVENT"), PatternModel::bars ("bars"),
                       PatternModel::step ("step"), PatternModel::length ("length"), PatternModel::chop ("chop"),
                       PatternModel::velocity ("velocity"), PatternModel::pitch ("pitch"), PatternModel::chance ("chance"),
                       PatternModel::repeats ("repeats"), PatternModel::random ("random"), PatternModel::slide ("slide");

PatternModel::PatternModel (juce::UndoManager* undo) : undoManager (undo)
{
    state.setProperty (bars, 1, nullptr);
}

void PatternModel::replaceState (const juce::ValueTree& newState)
{
    if (! newState.hasType (PATTERN))
        return;

    state.copyPropertiesAndChildrenFrom (newState, nullptr);
    setBars (getBars()); // clamps
    const auto steps = getNumSteps();

    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto e = state.getChild (i);
        const int s = e.getProperty (step, -1);
        if (! e.hasType (EVENT) || s < 0 || s >= steps || (int) e.getProperty (chop, 0) <= 0)
            state.removeChild (i, nullptr);
        else
            e.setProperty (length, juce::jlimit (1, steps - s, (int) e.getProperty (length, 1)), nullptr);
    }
}

int PatternModel::getBars() const
{
    return juce::jlimit (1, 8, (int) state.getProperty (bars, 1));
}

void PatternModel::setBars (int numBars)
{
    numBars = juce::jlimit (1, 8, numBars);
    state.setProperty (bars, numBars, undoManager);

    // Remove events beyond the new end; shorten those that cross it.
    const auto steps = numBars * PatternData::stepsPerBar;
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto e = state.getChild (i);
        const int s = e.getProperty (step, 0);
        if (s >= steps)
            state.removeChild (i, undoManager);
        else if ((int) e.getProperty (length, 1) > steps - s)
            e.setProperty (length, steps - s, undoManager);
    }
}

PatternEvent PatternModel::fromTree (const juce::ValueTree& t)
{
    PatternEvent e;
    e.step = t.getProperty (step, 0);
    e.length = std::max (1, (int) t.getProperty (length, 1));
    e.chopId = t.getProperty (chop, 0);
    e.velocity = juce::jlimit (0.05f, 1.0f, (float) t.getProperty (velocity, 0.85f));
    e.pitch = juce::jlimit (-PatternEvent::maxPitch, PatternEvent::maxPitch, (int) t.getProperty (pitch, 0));
    e.chance = juce::jlimit (0.05f, 1.0f, (float) t.getProperty (chance, 1.0f));
    e.repeats = juce::jlimit (1, PatternEvent::maxRepeats, (int) t.getProperty (repeats, 1));
    e.random = juce::jlimit (0, 2, (int) t.getProperty (random, 0));
    e.slide = (bool) t.getProperty (slide, false);
    return e;
}

std::vector<PatternEvent> PatternModel::getEvents() const
{
    std::vector<PatternEvent> out;
    for (const auto& c : state)
        out.push_back (fromTree (c));
    std::sort (out.begin(), out.end(), [] (const PatternEvent& a, const PatternEvent& b) { return a.step < b.step; });
    return out;
}

std::optional<PatternEvent> PatternModel::eventAt (int stepIndex) const
{
    for (const auto& e : getEvents())
        if (stepIndex >= e.step && stepIndex < e.step + e.length)
            return e;
    return std::nullopt;
}

juce::ValueTree PatternModel::findEvent (int stepIndex) const
{
    for (const auto& c : state)
        if ((int) c.getProperty (step, -1) == stepIndex)
            return c;
    return {};
}

void PatternModel::setEvent (int stepIndex, int chopId, int len, float vel)
{
    const auto steps = getNumSteps();
    if (stepIndex < 0 || stepIndex >= steps || chopId <= 0 || state.getNumChildren() >= PatternData::maxEvents)
        return;

    len = juce::jlimit (1, steps - stepIndex, len);

    // An earlier event held across this step is shortened so they do not overlap.
    for (auto c : state)
    {
        const int s = c.getProperty (step, 0);
        const int l = c.getProperty (length, 1);
        if (s < stepIndex && s + l > stepIndex)
            c.setProperty (length, stepIndex - s, undoManager);
    }

    // A new event gets `vel`; swapping the chop of an existing one keeps its expression
    // (velocity, pitch, chance, repeats).
    auto existing = findEvent (stepIndex);
    if (! existing.isValid())
    {
        existing = juce::ValueTree (EVENT);
        existing.setProperty (step, stepIndex, nullptr);
        existing.setProperty (velocity, juce::jlimit (0.05f, 1.0f, vel), nullptr);
        state.appendChild (existing, undoManager);
    }

    existing.setProperty (chop, chopId, undoManager);
    existing.setProperty (length, len, undoManager);
}

void PatternModel::setVelocity (int stepIndex, float v)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (velocity, juce::jlimit (0.05f, 1.0f, v), undoManager);
}

void PatternModel::setPitch (int stepIndex, int semitones)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (pitch, juce::jlimit (-PatternEvent::maxPitch, PatternEvent::maxPitch, semitones), undoManager);
}

void PatternModel::setChance (int stepIndex, float c)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (chance, juce::jlimit (0.05f, 1.0f, c), undoManager);
}

void PatternModel::setRepeats (int stepIndex, int r)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (repeats, juce::jlimit (1, PatternEvent::maxRepeats, r), undoManager);
}

void PatternModel::setRandom (int stepIndex, int mode)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (random, juce::jlimit (0, 2, mode), undoManager);
}

void PatternModel::setSlide (int stepIndex, bool shouldSlide)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        e.setProperty (slide, shouldSlide, undoManager);
}

void PatternModel::removeEventStartingAt (int stepIndex)
{
    if (auto e = findEvent (stepIndex); e.isValid())
        state.removeChild (e, undoManager);
}

void PatternModel::setLength (int stepIndex, int len)
{
    if (auto e = findEvent (stepIndex); e.isValid())
    {
        // Do not run into the next event.
        int limit = getNumSteps() - stepIndex;
        for (const auto& c : state)
        {
            const int s = c.getProperty (step, 0);
            if (s > stepIndex)
                limit = std::min (limit, s - stepIndex);
        }
        e.setProperty (length, juce::jlimit (1, limit, len), undoManager);
    }
}

std::vector<juce::ValueTree> PatternModel::setLengthOver (int stepIndex, int len)
{
    std::vector<juce::ValueTree> removed;
    auto e = findEvent (stepIndex);
    if (! e.isValid())
        return removed;

    len = juce::jlimit (1, getNumSteps() - stepIndex, len);
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto c = state.getChild (i);
        const int s = c.getProperty (step, 0);
        if (s > stepIndex && s < stepIndex + len)
        {
            removed.push_back (c.createCopy());
            state.removeChild (i, undoManager);
        }
    }
    e.setProperty (length, len, undoManager);
    return removed;
}

void PatternModel::restoreEvent (const juce::ValueTree& event)
{
    // Only into free space (not on or inside another event); it ends at the next event.
    const int s = event.getProperty (step, -1);
    if (s < 0 || s >= getNumSteps() || eventAt (s).has_value() || state.getNumChildren() >= PatternData::maxEvents)
        return;
    auto copy = event.createCopy();
    int limit = getNumSteps() - s;
    for (const auto& c : state)
        if (const int other = c.getProperty (step, 0); other > s)
            limit = std::min (limit, other - s);
    copy.setProperty (length, juce::jlimit (1, limit, (int) copy.getProperty (length, 1)), nullptr);
    state.appendChild (copy, undoManager);
}

void PatternModel::clear()
{
    state.removeAllChildren (undoManager);
}

void PatternModel::removeEventsForMissingChops (const std::vector<int>& existing)
{
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const int id = state.getChild (i).getProperty (chop, 0);
        if (std::find (existing.begin(), existing.end(), id) == existing.end())
            state.removeChild (i, undoManager);
    }
}

} // namespace kerf
