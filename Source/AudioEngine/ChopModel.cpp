#include "ChopModel.h"

#include <algorithm>

namespace kerf
{

ChopModel::ChopModel (juce::UndoManager* undo) : undoManager (undo)
{
    state.setProperty (IDs::nextId, 1, nullptr);
    state.setProperty (IDs::rootNote, defaultRootNote, nullptr);
    state.setProperty (IDs::sampleLength, 0, nullptr);
}

void ChopModel::replaceState (const juce::ValueTree& newState, bool clearUndo)
{
    if (! newState.hasType (IDs::CHOPS))
        return;

    // Copy into our existing tree so listeners attached to `state` stay valid.
    state.copyPropertiesAndChildrenFrom (newState, nullptr);

    // Sanitise everything that came from disk.
    const auto len = getSampleLength();
    int maxId = 0;

    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto child = state.getChild (i);
        if (! child.hasType (IDs::CHOP))
        {
            state.removeChild (i, nullptr);
            continue;
        }

        auto p = sanitised (fromTree (child), len);
        if (p.id <= 0 || p.length() < ChopLimits::minLength)
        {
            state.removeChild (i, nullptr);
            continue;
        }

        writeToTree (child, p, nullptr);
        maxId = std::max (maxId, p.id);
    }

    if ((int) state.getProperty (IDs::nextId) <= maxId)
        state.setProperty (IDs::nextId, maxId + 1, nullptr);

    if (undoManager != nullptr && clearUndo)
        undoManager->clearUndoHistory();
}

//==============================================================================
int64_t ChopModel::getSampleLength() const { return (int64_t) (juce::int64) state.getProperty (IDs::sampleLength, 0); }
int ChopModel::getRootNote() const          { return juce::jlimit (0, 127, (int) state.getProperty (IDs::rootNote, defaultRootNote)); }

void ChopModel::resetForSample (int64_t sampleLength, bool recordUndo)
{
    auto* um = recordUndo ? undoManager : nullptr;
    state.removeAllChildren (um);
    state.setProperty (IDs::sampleLength, (juce::int64) sampleLength, um);

    if (sampleLength >= ChopLimits::minLength)
        state.appendChild (createChopTree (0, sampleLength, getRootNote()), um);
}

std::vector<ChopParams> ChopModel::getChops() const
{
    std::vector<ChopParams> result;
    result.reserve ((size_t) state.getNumChildren());

    for (const auto& child : state)
        result.push_back (fromTree (child));

    std::stable_sort (result.begin(), result.end(),
                      [] (const ChopParams& a, const ChopParams& b) { return a.start < b.start || (a.start == b.start && a.id < b.id); });
    return result;
}

std::optional<ChopParams> ChopModel::getChop (int id) const
{
    if (auto t = findChop (id); t.isValid())
        return fromTree (t);
    return std::nullopt;
}

int ChopModel::chopIdAt (int64_t position) const
{
    // Prefer the shortest chop containing the position, so a small chop sitting on top
    // of a large one can still be picked.
    int bestId = 0;
    int64_t bestLength = std::numeric_limits<int64_t>::max();

    for (const auto& c : getChops())
        if (position >= c.start && position < c.end && c.length() < bestLength)
        {
            bestId = c.id;
            bestLength = c.length();
        }

    return bestId;
}

int ChopModel::chopIdForNote (int note) const
{
    for (const auto& child : state)
        if ((int) child.getProperty (IDs::note, -1) == note)
            return (int) child.getProperty (IDs::id);
    return 0;
}

//==============================================================================
int ChopModel::addChop (int64_t start, int64_t end)
{
    const auto len = getSampleLength();
    start = std::clamp<int64_t> (start, 0, len);
    end = std::clamp<int64_t> (end, 0, len);
    if (end < start) std::swap (start, end);

    if (end - start < ChopLimits::minLength || getNumChops() >= chopLimit())
        return 0;

    auto t = createChopTree (start, end, lowestFreeNote());
    state.appendChild (t, undoManager);
    return (int) t.getProperty (IDs::id);
}

int ChopModel::splitAt (int64_t position, int chopId)
{
    if (chopId == 0)
        chopId = chopIdAt (position);

    auto t = findChop (chopId);
    if (! t.isValid() || getNumChops() >= chopLimit())
        return 0;

    auto original = fromTree (t);

    // Each half must be at least the minimum length.
    if (position - original.start < ChopLimits::minLength || original.end - position < ChopLimits::minLength)
        return 0;

    // The right half is a new chop that inherits the sound settings of the original.
    auto right = original;
    right.start = position;

    auto newTree = createChopTree (position, original.end, lowestFreeNote());
    right.id = (int) newTree.getProperty (IDs::id);
    right.note = (int) newTree.getProperty (IDs::note);

    t.setProperty (IDs::end, (juce::int64) position, undoManager);
    writeToTree (newTree, right, nullptr);
    state.appendChild (newTree, undoManager);
    return right.id;
}

bool ChopModel::moveEdge (int chopId, ChopEdge edge, int64_t newPosition, bool linked)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    const auto c = fromTree (t);
    const auto len = getSampleLength();
    const auto oldPosition = edge == ChopEdge::start ? c.start : c.end;

    // Limits from this chop.
    int64_t lo = edge == ChopEdge::start ? 0 : c.start + ChopLimits::minLength;
    int64_t hi = edge == ChopEdge::start ? c.end - ChopLimits::minLength : len;

    // Neighbours touching the moved edge move with it, and constrain the range.
    std::vector<juce::ValueTree> partners;

    if (linked)
    {
        for (const auto& other : state)
        {
            const auto o = fromTree (other);
            if (o.id == chopId)
                continue;

            if (edge == ChopEdge::start && o.end == oldPosition)
            {
                partners.push_back (other);
                lo = std::max (lo, o.start + ChopLimits::minLength);
            }
            else if (edge == ChopEdge::end && o.start == oldPosition)
            {
                partners.push_back (other);
                hi = std::min (hi, o.end - ChopLimits::minLength);
            }
        }
    }

    if (hi < lo)
        return false;

    newPosition = std::clamp (newPosition, lo, hi);
    if (newPosition == oldPosition)
        return false;

    t.setProperty (edge == ChopEdge::start ? IDs::start : IDs::end, (juce::int64) newPosition, undoManager);

    for (auto& p : partners)
        p.setProperty (edge == ChopEdge::start ? IDs::end : IDs::start, (juce::int64) newPosition, undoManager);

    return true;
}

bool ChopModel::moveChop (int chopId, int64_t newStart)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    const auto c = fromTree (t);
    newStart = std::clamp<int64_t> (newStart, 0, std::max<int64_t> (0, getSampleLength() - c.length()));

    if (newStart == c.start)
        return false;

    t.setProperty (IDs::start, (juce::int64) newStart, undoManager);
    t.setProperty (IDs::end, (juce::int64) (newStart + c.length()), undoManager);
    return true;
}

bool ChopModel::removeChop (int chopId)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    state.removeChild (t, undoManager);
    return true;
}

bool ChopModel::mergeWithNext (int chopId)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    const auto c = fromTree (t);

    // The next chop is the one whose start is closest at or after this chop's end
    // (falling back to the next start after this chop's start).
    juce::ValueTree next;
    int64_t bestStart = std::numeric_limits<int64_t>::max();

    for (const auto& other : state)
    {
        const auto o = fromTree (other);
        if (o.id != chopId && o.start > c.start && o.start < bestStart)
        {
            bestStart = o.start;
            next = other;
        }
    }

    if (! next.isValid())
        return false;

    const auto n = fromTree (next);
    t.setProperty (IDs::end, (juce::int64) std::max (c.end, n.end), undoManager);
    state.removeChild (next, undoManager);
    return true;
}

bool ChopModel::replaceWithBoundaries (const std::vector<int64_t>& boundariesIn)
{
    const auto len = getSampleLength();

    std::vector<int64_t> b;
    for (auto v : boundariesIn)
        b.push_back (std::clamp<int64_t> (v, 0, len));

    std::sort (b.begin(), b.end());

    // Drop boundaries that would create chops shorter than the minimum.
    std::vector<int64_t> clean;
    for (auto v : b)
        if (clean.empty() || v - clean.back() >= ChopLimits::minLength)
            clean.push_back (v);

    // Keep the final boundary (usually the region end) by absorbing a too-short last sliver.
    if (clean.size() >= 2 && ! b.empty() && b.back() > clean.back())
        clean.back() = b.back();

    if (clean.size() < 2)
        return false;

    // At most one chop per key; with more boundaries, the last chop runs to the end.
    const auto numNew = std::min<int> ((int) clean.size() - 1, chopLimit());

    state.removeAllChildren (undoManager);

    const auto root = getRootNote();
    for (int i = 0; i < numNew; ++i)
    {
        const auto note = root + i <= 127 ? root + i : -1;
        const auto end = i == numNew - 1 ? clean.back() : clean[(size_t) i + 1];
        state.appendChild (createChopTree (clean[(size_t) i], end, note), undoManager);
    }

    return true;
}

std::vector<ChopSpan> ChopModel::cleanSpans (std::vector<ChopSpan> spans, int limit, int64_t maxGap) const
{
    if (limit <= 0)
        limit = chopLimit();
    const auto len = getSampleLength();
    for (auto& s : spans)
    {
        s.start = std::clamp<int64_t> (s.start, 0, len);
        s.end = std::clamp<int64_t> (s.end, 0, len);
    }
    std::sort (spans.begin(), spans.end(), [] (const ChopSpan& a, const ChopSpan& b) { return a.start < b.start; });

    std::vector<ChopSpan> clean;
    for (auto s : spans)
    {
        if (! clean.empty())
            s.start = std::max (s.start, clean.back().end);
        if (s.end - s.start >= ChopLimits::minLength)
            clean.push_back (s);
    }

    // At most one per key: the last one reaches over the chops after it (up to a silence).
    if ((int) clean.size() > limit)
    {
        auto end = clean[(size_t) limit - 1].end;
        for (auto i = (size_t) limit; i < clean.size() && clean[i].start - end < maxGap; ++i)
            end = clean[i].end;
        clean.resize ((size_t) limit);
        clean.back().end = end;
    }
    return clean;
}

bool ChopModel::replaceWithSpans (const std::vector<ChopSpan>& spans, int64_t maxGap)
{
    const auto clean = cleanSpans (spans, 0, maxGap);
    if (clean.empty())
        return false;

    state.removeAllChildren (undoManager);
    const auto root = getRootNote();
    for (size_t i = 0; i < clean.size(); ++i)
        state.appendChild (createChopTree (clean[i].start, clean[i].end, root + (int) i <= 127 ? root + (int) i : -1), undoManager);
    return true;
}

bool ChopModel::subdivideIntoSpans (int chopId, const std::vector<ChopSpan>& spans)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    const auto original = fromTree (t);
    std::vector<ChopSpan> inside;
    for (auto s : spans)
    {
        s.start = std::max (s.start, original.start);
        s.end = std::min (s.end, original.end);
        if (s.end > s.start)
            inside.push_back (s);
    }

    // The other chops stay: room for the rest of the keys.
    const auto clean = cleanSpans (inside, std::max (1, chopLimit() - getNumChops() + 1));
    if (clean.empty() || (clean.size() == 1 && clean.front() == ChopSpan { original.start, original.end }))
        return false;

    t.setProperty (IDs::start, (juce::int64) clean.front().start, undoManager);
    t.setProperty (IDs::end, (juce::int64) clean.front().end, undoManager);
    for (size_t i = 1; i < clean.size(); ++i)
    {
        auto copy = original;
        copy.start = clean[i].start;
        copy.end = clean[i].end;
        auto tree = createChopTree (copy.start, copy.end, lowestFreeNote());
        copy.id = (int) tree.getProperty (IDs::id);
        copy.note = (int) tree.getProperty (IDs::note);
        writeToTree (tree, copy, nullptr);
        state.appendChild (tree, undoManager);
    }
    return true;
}

bool ChopModel::setNote (int chopId, int note)
{
    auto t = findChop (chopId);
    if (! t.isValid() || note < -1 || note > 127)
        return false;

    const int oldNote = t.getProperty (IDs::note, -1);
    if (oldNote == note)
        return false;

    if (note >= 0)
        if (auto other = findChop (chopIdForNote (note)); other.isValid())
            other.setProperty (IDs::note, oldNote, undoManager); // swap

    t.setProperty (IDs::note, note, undoManager);
    return true;
}

void ChopModel::setRootNote (int note)
{
    state.setProperty (IDs::rootNote, juce::jlimit (0, 127, note), undoManager);
}

void ChopModel::remapByPosition()
{
    const auto chops = getChops();
    const auto root = getRootNote();

    for (size_t i = 0; i < chops.size(); ++i)
    {
        const int note = root + (int) i <= 127 ? root + (int) i : -1;
        findChop (chops[i].id).setProperty (IDs::note, note, undoManager);
    }
}

bool ChopModel::setParam (int chopId, const juce::Identifier& param, const juce::var& value)
{
    auto t = findChop (chopId);
    if (! t.isValid())
        return false;

    auto p = fromTree (t);

    if      (param == IDs::mode)      p.mode = (PlayMode) (int) value;
    else if (param == IDs::pitch)     p.pitchSemitones = (float) value;
    else if (param == IDs::fine)      p.fineCents = (float) value;
    else if (param == IDs::reverse)   p.reverse = (bool) value;
    else if (param == IDs::gain)      p.gainDb = (float) value;
    else if (param == IDs::pan)       p.pan = (float) value;
    else if (param == IDs::fadeIn)    p.fadeInMs = (float) value;
    else if (param == IDs::fadeOut)   p.fadeOutMs = (float) value;
    else if (param == IDs::spaceSend) p.spaceSend = (float) value;
    else if (param == IDs::grainSend) p.grainSend = (float) value;
    else if (param == IDs::choke)     p.chokeGroup = (int) value;
    else if (param == IDs::decay)     p.decayMs = (float) value;
    else if (param == IDs::sustain)   p.sustain = (float) value;
    else if (param == IDs::note)      return setNote (chopId, (int) value);
    else if (param == IDs::start)     return moveEdge (chopId, ChopEdge::start, (int64_t) (juce::int64) value, false);
    else if (param == IDs::end)       return moveEdge (chopId, ChopEdge::end, (int64_t) (juce::int64) value, false);
    else return false;

    writeToTree (t, sanitised (p, getSampleLength()), undoManager);
    return true;
}

bool ChopModel::setChop (const ChopParams& params)
{
    auto t = findChop (params.id);
    if (! t.isValid())
        return false;

    auto p = sanitised (params, getSampleLength());
    const auto oldNote = (int) t.getProperty (IDs::note, -1);
    p.note = oldNote; // notes only change through setNote() so swaps stay consistent
    writeToTree (t, p, undoManager);

    if (params.note != oldNote)
        setNote (params.id, params.note);

    return true;
}

//==============================================================================
ChopParams ChopModel::fromTree (const juce::ValueTree& t)
{
    ChopParams p;
    p.id             = t.getProperty (IDs::id, 0);
    p.start          = (juce::int64) t.getProperty (IDs::start, 0);
    p.end            = (juce::int64) t.getProperty (IDs::end, 0);
    p.note           = t.getProperty (IDs::note, -1);
    p.mode           = (PlayMode) (int) t.getProperty (IDs::mode, (int) PlayMode::gate);
    p.pitchSemitones = t.getProperty (IDs::pitch, 0.0f);
    p.fineCents      = t.getProperty (IDs::fine, 0.0f);
    p.reverse        = t.getProperty (IDs::reverse, false);
    p.gainDb         = t.getProperty (IDs::gain, 0.0f);
    p.pan            = t.getProperty (IDs::pan, 0.0f);
    p.fadeInMs       = t.getProperty (IDs::fadeIn, 1.0f);
    p.fadeOutMs      = t.getProperty (IDs::fadeOut, 8.0f);
    p.spaceSend      = t.getProperty (IDs::spaceSend, 1.0f);
    p.grainSend      = t.getProperty (IDs::grainSend, 1.0f);
    p.chokeGroup     = t.getProperty (IDs::choke, 0);
    p.decayMs        = t.getProperty (IDs::decay, 300.0f);
    p.sustain        = t.getProperty (IDs::sustain, 1.0f);
    return p;
}

void ChopModel::writeToTree (juce::ValueTree& t, const ChopParams& p, juce::UndoManager* um)
{
    // setProperty is a no-op (and creates no undo step) when the value is unchanged.
    t.setProperty (IDs::id, p.id, um);
    t.setProperty (IDs::start, (juce::int64) p.start, um);
    t.setProperty (IDs::end, (juce::int64) p.end, um);
    t.setProperty (IDs::note, p.note, um);
    t.setProperty (IDs::mode, (int) p.mode, um);
    t.setProperty (IDs::pitch, p.pitchSemitones, um);
    t.setProperty (IDs::fine, p.fineCents, um);
    t.setProperty (IDs::reverse, p.reverse, um);
    t.setProperty (IDs::gain, p.gainDb, um);
    t.setProperty (IDs::pan, p.pan, um);
    t.setProperty (IDs::fadeIn, p.fadeInMs, um);
    t.setProperty (IDs::fadeOut, p.fadeOutMs, um);
    t.setProperty (IDs::spaceSend, p.spaceSend, um);
    t.setProperty (IDs::grainSend, p.grainSend, um);
    t.setProperty (IDs::choke, p.chokeGroup, um);
    t.setProperty (IDs::decay, p.decayMs, um);
    t.setProperty (IDs::sustain, p.sustain, um);
}

ChopParams ChopModel::sanitised (ChopParams p, int64_t sampleLength)
{
    auto finiteOr = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };

    p.start = std::clamp<int64_t> (p.start, 0, std::max<int64_t> (0, sampleLength));
    p.end   = std::clamp<int64_t> (p.end, 0, std::max<int64_t> (0, sampleLength));
    if (p.end < p.start) std::swap (p.start, p.end);

    p.note = (p.note >= 0 && p.note <= 127) ? p.note : -1;
    p.mode = (PlayMode) std::clamp ((int) p.mode, 0, 2);
    p.pitchSemitones = std::clamp (finiteOr (p.pitchSemitones, 0.0f), ChopLimits::minPitch, ChopLimits::maxPitch);
    p.fineCents      = std::clamp (finiteOr (p.fineCents, 0.0f), ChopLimits::minFine, ChopLimits::maxFine);
    p.gainDb         = std::clamp (finiteOr (p.gainDb, 0.0f), ChopLimits::minGainDb, ChopLimits::maxGainDb);
    p.pan            = std::clamp (finiteOr (p.pan, 0.0f), -1.0f, 1.0f);
    p.fadeInMs       = std::clamp (finiteOr (p.fadeInMs, 1.0f), 0.0f, ChopLimits::maxFadeInMs);
    p.fadeOutMs      = std::clamp (finiteOr (p.fadeOutMs, 8.0f), 0.0f, ChopLimits::maxFadeOutMs);
    p.spaceSend      = std::clamp (finiteOr (p.spaceSend, 1.0f), 0.0f, 1.0f);
    p.grainSend      = std::clamp (finiteOr (p.grainSend, 1.0f), 0.0f, 1.0f);
    p.chokeGroup     = std::clamp (p.chokeGroup, 0, ChopLimits::maxChokeGroup);
    p.decayMs        = std::clamp (finiteOr (p.decayMs, 300.0f), ChopLimits::minDecayMs, ChopLimits::maxDecayMs);
    p.sustain        = std::clamp (finiteOr (p.sustain, 1.0f), 0.0f, 1.0f);
    return p;
}

//==============================================================================
juce::ValueTree ChopModel::findChop (int id) const
{
    if (id <= 0)
        return {};

    for (const auto& child : state)
        if ((int) child.getProperty (IDs::id) == id)
            return child;

    return {};
}

int ChopModel::allocateId()
{
    // Not undoable on purpose: ids must never be reused, even after undo.
    const int id = state.getProperty (IDs::nextId, 1);
    state.setProperty (IDs::nextId, id + 1, nullptr);
    return id;
}

int ChopModel::lowestFreeNote() const
{
    std::array<bool, 128> used {};
    for (const auto& child : state)
    {
        const int n = child.getProperty (IDs::note, -1);
        if (n >= 0 && n < 128)
            used[(size_t) n] = true;
    }

    for (int n = getRootNote(); n < 128; ++n)
        if (! used[(size_t) n])
            return n;

    for (int n = getRootNote(); --n >= 0;)
        if (! used[(size_t) n])
            return n;

    return -1;
}

juce::ValueTree ChopModel::createChopTree (int64_t start, int64_t end, int note)
{
    ChopParams p;
    p.id = allocateId();
    p.start = start;
    p.end = end;
    p.note = note;

    juce::ValueTree t (IDs::CHOP);
    writeToTree (t, p, nullptr);
    return t;
}

} // namespace kerf
