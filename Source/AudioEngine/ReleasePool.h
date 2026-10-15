#pragma once

#include <juce_events/juce_events.h>

namespace kerf
{

/** Keeps a reference to every object handed to the audio thread and deletes them on the
    message thread once nobody else holds them.

    Why: if the audio thread dropped the *last* reference to a sample buffer, the buffer
    would be freed inside the audio callback (an unbounded, potentially blocking
    operation). The pool always holds one extra reference, so the audio thread can never
    be the last owner. A timer frees objects whose only remaining owner is the pool. */
class ReleasePool final : private juce::Timer
{
public:
    ReleasePool() { startTimer (500); }
    ~ReleasePool() override { stopTimer(); }

    void add (juce::ReferenceCountedObject* object)
    {
        if (object == nullptr)
            return;

        // Once only: the pool lets go when its reference is the last one, and two of its own
        // would keep the object alive for ever (each loaded sample was added again with
        // every change, and no sample was ever freed).
        const juce::ScopedLock sl (lock);
        pool.addIfNotAlreadyThere (object);
    }

    /** Frees anything only the pool still references. Safe to call from any non-audio thread. */
    void collect()
    {
        const juce::ScopedLock sl (lock);

        for (int i = pool.size(); --i >= 0;)
            if (pool.getObjectPointerUnchecked (i)->getReferenceCount() == 1)
                pool.remove (i);
    }

    int size() const { const juce::ScopedLock sl (lock); return pool.size(); }

private:
    void timerCallback() override { collect(); }

    juce::CriticalSection lock;
    juce::ReferenceCountedArray<juce::ReferenceCountedObject> pool;
};

/** Single-slot handoff of a reference-counted object from a writer (message thread)
    to a reader (audio thread).

    - publish() is called on a non-realtime thread and may briefly block.
    - acquire() is called on the audio thread and never blocks: if the writer holds the
      lock at that instant, the reader keeps the object it already had and tries again
      next block. */
template <typename ObjectType>
class RealtimeHandoff
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<ObjectType>;

    explicit RealtimeHandoff (ReleasePool& poolToUse) : pool (poolToUse) {}

    void publish (Ptr newObject)
    {
        pool.add (newObject.get());
        const juce::SpinLock::ScopedLockType sl (lock);
        latest = std::move (newObject);
        ++version;
    }

    /** Audio thread: updates `current` if a newer object has been published.
        Returns true if `current` changed. */
    bool acquire (Ptr& current, uint32_t& currentVersion) noexcept
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);

        if (! sl.isLocked() || version == currentVersion)
            return false;

        current = latest; // reference-count increment only; the pool guarantees no free here
        currentVersion = version;
        return true;
    }

    /** Non-realtime read of the latest published object. */
    Ptr getLatest() const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        return latest;
    }

private:
    ReleasePool& pool;
    mutable juce::SpinLock lock;
    Ptr latest;
    uint32_t version = 0;
};

} // namespace kerf
