#include "SamplerEngine.h"

#include <limits>

namespace kerf
{

double quantizeBeats (TriggerQuantize q) noexcept
{
    switch (q)
    {
        case TriggerQuantize::off:       return 0.0;
        case TriggerQuantize::sixteenth: return 0.25;
        case TriggerQuantize::eighth:    return 0.5;
        case TriggerQuantize::quarter:   return 1.0;
        case TriggerQuantize::bar:       return 4.0; // replaced by the host's bar length when known
    }
    return 0.0;
}

void SamplerEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    for (auto& v : voices)
        v.prepare (newSampleRate);

    for (auto& d : display)
        d.chopId.store (0);

    numPending = 0;
    sustainPedal = false;
    heldBySustain.fill (false);
}

void SamplerEngine::setSnapshot (const EngineSnapshot* newSnapshot)
{
    snapshot = newSnapshot;

    for (auto& v : voices)
    {
        if (! v.isActive())
            continue;

        if (snapshot == nullptr)
        {
            v.kill();
            continue;
        }

        if (const auto* c = snapshot->chopWithId (v.getChopId()))
            v.updateChop (*c);
        else
            v.kill(); // chop deleted while sounding
    }
}

void SamplerEngine::allNotesOff (bool immediate)
{
    for (auto& v : voices)
        immediate ? v.kill() : v.noteOff();

    numPending = 0;
    sustainPedal = false;
    heldBySustain.fill (false);
}

int SamplerEngine::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        n += v.isActive() ? 1 : 0;
    return n;
}

//==============================================================================
void SamplerEngine::process (const VoiceBusses& out, int numSamples, const juce::MidiBuffer& midi,
                             const EngineSettings& settings, const TransportInfo& transport)
{
    int position = 0;
    auto it = midi.cbegin();
    const auto end = midi.cend();

    while (position < numSamples)
    {
        // Next point where something happens: a MIDI event or a quantised note-on.
        int nextEvent = numSamples;

        if (it != end)
            nextEvent = std::min (nextEvent, juce::jlimit (position, numSamples, (*it).samplePosition));

        for (int i = 0; i < numPending; ++i)
            nextEvent = std::min (nextEvent, position + (int) std::min<int64_t> (numSamples - position, pending[(size_t) i].samplesUntilStart));

        // A queued chop starts the sample the one before it ends.
        nextEvent = std::min (nextEvent, queueDueAt (position, settings));

        if (nextEvent > position)
        {
            renderVoices (out, position, nextEvent - position, settings);

            for (int i = 0; i < numPending; ++i)
                pending[(size_t) i].samplesUntilStart -= (nextEvent - position);

            position = nextEvent;
        }

        if (queuedNext != 0 && queueDueAt (position, settings) <= position)
        {
            // The one before is at its end: it ends here (with LOOP it would go round again).
            for (auto& v : voices)
                if (v.isActive() && v.isPlayOnce() && v.getChopId() == queuedAfter)
                    v.kill();
            const auto next = queuedNext;
            queuedAfter = queuedNext = 0;
            previewChop (next, queuedVelocity, true, settings);
        }

        // Fire quantised notes that are due now.
        for (int i = 0; i < numPending;)
        {
            if (pending[(size_t) i].samplesUntilStart <= 0)
            {
                const auto p = pending[(size_t) i];
                pending[(size_t) i] = pending[(size_t) --numPending];
                noteOn (p.note, p.velocity, settings, false, p.step);
                if (p.released)
                    noteOff (p.note);
            }
            else
            {
                ++i;
            }
        }

        // Handle all MIDI events at this position.
        while (it != end && juce::jlimit (0, numSamples, (*it).samplePosition) <= position)
        {
            const auto meta = *it;
            handleMidi (meta.getMessage(), position, settings, transport);
            ++it;
        }

        if (it == end && numPending == 0 && queuedNext == 0 && position < numSamples)
        {
            renderVoices (out, position, numSamples - position, settings);
            position = numSamples;
        }
    }

    // Publish voice positions for the UI.
    for (size_t i = 0; i < voices.size(); ++i)
    {
        const auto& v = voices[i];
        display[i].chopId.store (v.isActive() ? v.getChopId() : 0, std::memory_order_relaxed);
        display[i].position.store (v.getDisplayPosition(), std::memory_order_relaxed);
        display[i].level.store (v.isActive() ? v.getCurrentLevel() : 0.0f, std::memory_order_relaxed);

        // Grains (only when this voice actually uses them).
        auto& d = display[i];
        const auto amount = v.isActive() ? v.getGrainAmount() : 0.0f;
        d.grainAmount.store (amount, std::memory_order_relaxed);
        int n = 0;
        if (amount > 0.001f)
        {
            v.forEachGrainDisplay ([&d, &n] (double pos, float lvl, float semis)
            {
                if (n < VoiceDisplay::maxGrains)
                {
                    d.grainPosition[(size_t) n].store (pos, std::memory_order_relaxed);
                    d.grainLevel[(size_t) n].store (lvl, std::memory_order_relaxed);
                    d.grainPitch[(size_t) n].store (semis, std::memory_order_relaxed);
                    ++n;
                }
            });
            d.sprayCentre.store (v.getSprayCentreDisplay(), std::memory_order_relaxed);
            d.sprayHalfWidth.store (v.getSprayHalfWidthDisplay(), std::memory_order_relaxed);
        }
        d.numGrains.store (n, std::memory_order_release);
    }
}

void SamplerEngine::queuePreview (int afterId, int nextId, float velocity, const EngineSettings&)
{
    queuedAfter = nextId != 0 ? afterId : 0;
    queuedNext = nextId;
    queuedVelocity = velocity;
}

int SamplerEngine::queueDueAt (int position, const EngineSettings& settings) const
{
    constexpr int never = std::numeric_limits<int>::max();
    if (queuedNext == 0 || settings.previewsPaused)
        return never;
    for (const auto& v : voices)
        if (v.isActive() && v.isPlayOnce() && v.getChopId() == queuedAfter && v.getStage() == SampleVoice::Stage::playing)
            return position + (int) std::min (1.0e9, std::ceil (v.framesUntilEnd (settings.voice)));
    return position; // it has ended (or was stopped): now
}

void SamplerEngine::renderVoices (const VoiceBusses& out, int start, int num, const EngineSettings& settings)
{
    for (auto& v : voices)
    {
        if (! v.isActive())
            continue;
        if (v.isPlayOnce())
        {
            // LOOP follows the toolbar, also while it plays; a chop with another queued after it
            // plays to its end once more instead.
            v.setPreviewLoop (settings.previewLoop && ! (queuedNext != 0 && v.getChopId() == queuedAfter));
            if (settings.previewsPaused)
                continue; // paused: holds its place until PLAY resumes it
        }

        // Live pad automation (tune, gain, pan, sends, fades, reverse, mode).
        if (settings.pads != nullptr)
            if (const auto* pad = settings.pads->forNote (v.getNote()))
                v.applyPad (*pad);

        v.render (out, start, num, settings.voice);
    }
}

void SamplerEngine::handleMidi (const juce::MidiMessage& m, int samplePos, const EngineSettings& settings, const TransportInfo& transport)
{
    if (m.isNoteOn())
    {
        const auto note = m.getNoteNumber();
        const auto velocity = m.getFloatVelocity();
        const auto step = m.getChannel() == patternChannel ? std::exchange (patternStep, StepPitch {}) : StepPitch {};

        const auto q = quantizeBeats (settings.quantize) * (settings.quantize == TriggerQuantize::bar ? transport.beatsPerBar / 4.0 : 1.0);

        if (q > 0.0 && transport.isPlaying && transport.bpm > 0.0 && numPending < (int) pending.size())
        {
            const auto samplesPerBeat = 60.0 / transport.bpm * sampleRate;
            const auto ppq = transport.ppqAtBlockStart + samplePos / samplesPerBeat;
            const auto gridPpq = std::ceil (ppq / q - 1e-9) * q;
            const auto prevGridPpq = gridPpq - q;

            // Forgiving: a note slightly late (within 30 ms) of a grid line plays immediately.
            const auto lateBy = (ppq - prevGridPpq) * 60.0 / transport.bpm;
            if (lateBy > 0.03)
            {
                pending[(size_t) numPending++] = { note, velocity, (int64_t) std::llround ((gridPpq - ppq) * samplesPerBeat), false, step };
                return;
            }
        }

        noteOn (note, velocity, settings, m.getChannel() == previewOnceChannel, step);
    }
    else if (m.isPitchWheel() && m.getChannel() == patternChannel)
    {
        patternStep.transpose = (int) std::lround ((m.getPitchWheelValue() - 8192) / 256.0); // PatternPlayer::pitchToWheel
    }
    else if (m.isController() && m.getChannel() == patternChannel && m.getControllerNumber() == 102) // PatternPlayer::glideFromController
    {
        patternStep.glide = true;
        patternStep.glideFrom = m.getControllerValue() - 64;
    }
    else if (m.isController() && m.getChannel() == patternChannel && m.getControllerNumber() == 103) // PatternPlayer::glideTimeController
    {
        patternStep.glideSeconds = (float) m.getControllerValue() * 0.01f;
    }
    else if (m.isNoteOff() && m.getChannel() == previewOnceChannel)
    {
        // Stop a Space-bar preview of this note (with its fade-out).
        for (auto& v : voices)
            if (v.isActive() && v.getNote() == m.getNoteNumber() && v.isPlayOnce())
                v.stopPreview();
    }
    else if (m.isNoteOff())
    {
        const auto note = m.getNoteNumber();

        // A pending (quantised) note remembers it was released.
        for (int i = 0; i < numPending; ++i)
            if (pending[(size_t) i].note == note)
                pending[(size_t) i].released = true;

        if (sustainPedal)
            heldBySustain[(size_t) note] = true;
        else
            noteOff (note);
    }
    else if (m.isSustainPedalOn())
    {
        sustainPedal = true;
    }
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (int n = 0; n < 128; ++n)
            if (std::exchange (heldBySustain[(size_t) n], false))
                noteOff (n);
    }
    else if (m.isAllNotesOff())
    {
        allNotesOff (false);
    }
    else if (m.isAllSoundOff())
    {
        allNotesOff (true);
    }
}

void SamplerEngine::noteOn (int note, float velocity, const EngineSettings& settings, bool playOnce, StepPitch step)
{
    if (snapshot == nullptr || snapshot->sample == nullptr)
        return;

    const auto* chop = snapshot->chopForNote (note);
    if (chop == nullptr)
        return;
    startChop (*chop, snapshot->chopIndexFor (note), note, velocity, settings, playOnce, step);
}

void SamplerEngine::previewChop (int chopId, float velocity, bool playOnce, const EngineSettings& settings, double from)
{
    if (snapshot == nullptr || snapshot->sample == nullptr)
        return;

    // The whole sample, as one chop with the default sound (no pad, no MIDI note).
    if (chopId == wholeSampleId)
    {
        if (velocity > 0.0f)
        {
            ChopParams whole;
            whole.id = wholeSampleId;
            whole.start = 0;
            whole.end = snapshot->sample->getNumSamples();
            whole.mode = PlayMode::oneShot;
            startChop (whole, -1, wholeSampleNote, velocity, settings, playOnce, {}, from);
        }
        else
        {
            for (auto& v : voices)
                if (v.isActive() && v.getNote() == wholeSampleNote)
                {
                    if (playOnce && v.isPlayOnce()) v.stopPreview();
                    else if (! v.isReleasing())      v.noteOff();
                }
        }
        return;
    }

    for (int i = 0; i < snapshot->numChops; ++i)
    {
        const auto& chop = snapshot->chops[(size_t) i];
        if (chop.id != chopId)
            continue;

        // A chop on a note plays as that note; one beyond the 128 MIDI notes gets its own id.
        const auto voiceNote = chop.note >= 0 ? chop.note : previewNoteBase + i;
        if (velocity > 0.0f)
            startChop (chop, i, voiceNote, velocity, settings, playOnce, {}, from);
        else
            for (auto& v : voices)
                if (v.isActive() && v.getNote() == voiceNote)
                {
                    if (playOnce && v.isPlayOnce()) v.stopPreview();
                    else if (! v.isReleasing())      v.noteOff();
                }
        return;
    }
}

bool SamplerEngine::isPreviewPlaying (bool paused) const noexcept
{
    if (paused)
        return false;
    for (const auto& v : voices)
        if (v.isActive() && v.isPlayOnce() && ! v.isReleasing())
            return true;
    return false;
}

void SamplerEngine::startChop (const ChopParams& chopRef, int chopIndex, int note, float velocity, const EngineSettings& settings,
                               bool playOnce, StepPitch step, double from)
{
    const auto* chop = &chopRef;
    if (note >= 0 && note < 128)
        heldBySustain[(size_t) note] = false;

    // Voice-transition policy.
    for (auto& v : voices)
    {
        if (! v.isActive() || v.getStage() == SampleVoice::Stage::killing)
            continue;

        const bool sameNote = v.getNote() == note;
        const bool choked = chop->chokeGroup != 0 && v.getChokeGroup() == chop->chokeGroup;

        if (settings.retrigger == RetriggerMode::mono
            || (settings.retrigger == RetriggerMode::sameNoteCuts && sameNote)
            || choked)
            v.kill();
    }

    SampleData::Ptr rendered;
    if (snapshot->stretchMode)
        if (chopIndex >= 0 && chopIndex < maxChops)
            rendered = snapshot->rendered[(size_t) chopIndex];

    // Pad parameters (automatable) override the chop model's sound settings.
    auto chopToPlay = *chop;
    if (settings.pads != nullptr)
        if (const auto* pad = settings.pads->forNote (chopToPlay.note))
            pad->applyTo (chopToPlay);

    if (auto* voice = findVoiceToUse (settings))
    {
        voice->start (snapshot->sample, chopToPlay, note, velocity, ++ageCounter, rendered, playOnce, (float) step.transpose, from);
        if (step.glide)
            voice->glideFrom ((float) step.glideFrom, step.glideSeconds);
        display[(size_t) (voice - voices.data())].triggers.fetch_add (1, std::memory_order_relaxed);
    }
}

void SamplerEngine::noteOff (int note)
{
    for (auto& v : voices)
        if (v.isActive() && v.getNote() == note && ! v.isReleasing())
            v.noteOff();
}

SampleVoice* SamplerEngine::findVoiceToUse (const EngineSettings& settings)
{
    const int limit = juce::jlimit (1, maxPolyphony, settings.maxVoices);

    int sounding = 0;
    for (const auto& v : voices)
        sounding += (v.isActive() && v.getStage() != SampleVoice::Stage::killing) ? 1 : 0;

    // Over the limit: steal. Prefer the oldest releasing voice, then the oldest voice.
    while (sounding >= limit)
    {
        SampleVoice* victim = nullptr;

        for (auto& v : voices)
        {
            if (! v.isActive() || v.getStage() == SampleVoice::Stage::killing)
                continue;

            if (victim == nullptr
                || (v.isReleasing() && ! victim->isReleasing())
                || (v.isReleasing() == victim->isReleasing() && v.getAge() < victim->getAge()))
                victim = &v;
        }

        if (victim == nullptr)
            break;

        victim->kill();
        --sounding;
    }

    for (auto& v : voices)
        if (! v.isActive())
            return &v;

    // Every physical voice is busy (many fading tails): take the quietest fading one.
    SampleVoice* quietest = nullptr;
    for (auto& v : voices)
        if (quietest == nullptr || v.getCurrentLevel() < quietest->getCurrentLevel())
            quietest = &v;

    return quietest;
}

} // namespace kerf
