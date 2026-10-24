#include "FactoryPresets.h"
#include "Parameters.h"
#include "Effects/TimeShaper.h"
#include "Effects/PingPongDelay.h"
#include <array>
#include <map>

namespace kerf::Factory
{

namespace
{
    // EQ point types (EqProcessor::Type).
    constexpr int bell = 0, lowShelf = 1, highShelf = 2, lowCut = 3, highCut = 4, notch = 5;
    // Mode indices.
    constexpr float warm = 0.0f, fuzz = 1.0f, crush = 2.0f;   // DIRT
    constexpr float tape = 0.0f, chorus = 1.0f;               // WOBBLE
    constexpr float quarter = 0.0f, eighth = 1.0f, sixteenth = 2.0f; // PUMP
    constexpr float halfBar = 0.0f, oneBar = 1.0f, twoBars = 2.0f;  // TIME cycle
    // DELAY: its note value (PingPongDelay::Time) and mode; without them a preset echoes on dotted 1/8s, ping-pong.
    constexpr float t16 = 0.0f, t8T = 1.0f, t8 = 2.0f, t8D = 3.0f, t4T = 4.0f, t4 = 5.0f, t4D = 6.0f, t2 = 7.0f;
    constexpr float pingPong = 0.0f, stereo = 1.0f;
    constexpr float hard = 0.0f, soft = 0.5f, pluck = 1.0f;   // grain SHAPE
    constexpr float octaves = 1.0f, fifths = 2.0f, chord = 3.0f; // grain RANDOM scale
    // Swing as a parameter: 0 = straight (50 %) .. 1 = 75 %.
    constexpr float swingAt (float percent) { return (percent - 50.0f) / 25.0f; }
    // TONE: negative = low-pass, positive = high-pass (see ToneProcessor's cutoff curves).

    /** The TIME slot parameter's value for a factory slot, by its name (-1 if there is none:
        the snapshot test then reports the preset). */
    float slotOf (const char* name)
    {
        for (int k = 0; k < TimeFactory::numSlots; ++k)
            if (juce::String (TimeFactory::name (k)) == name)
                return (float) (k + 1);
        jassertfalse;
        return -1.0f;
    }

    using namespace kerf::Params;
}

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> all {
        // --- BEATS ------------------------------------------------------------------
        { "Boom Bap Dust", "BEATS", "Warm, dusty and laid back: saturation, a rounded top, MPC-style swing",
          { { drive, 0.32f }, { dirtMode, warm }, { filter, -0.22f }, { resonance, 0.15f }, { space, 0.12f }, { spaceTime, t8 }, { spaceMode, stereo },
            { wobble, 0.08f }, { wobbleMode, tape }, { Params::swing, swingAt (58.5f) }, { humanize, 0.25f } },
          { { lowCut, 32.0f, 0.0f, 0.7f }, { lowShelf, 110.0f, 2.5f, 0.7f }, { highShelf, 9000.0f, -2.0f, 0.7f } } },
        { "Trap Knock", "BEATS", "Hard and modern: a big low end, scooped mids, a crisp top, a hi-hat roll every two bars",
          { { drive, 0.18f }, { dirtMode, warm }, { humanize, 0.08f },
            { timeSlot, slotOf ("FILL 16") }, { timeLength, twoBars }, { timeMix, 0.8f } },
          { { lowShelf, 65.0f, 4.0f, 0.7f }, { bell, 380.0f, -3.0f, 1.2f }, { highShelf, 9000.0f, 2.5f, 0.7f } } },
        { "House Pump", "BEATS", "Four to the floor: the sidechain pump on every beat, an airy top",
          { { pump, 0.65f }, { pumpMode, quarter }, { space, 0.2f }, { Params::swing, swingAt (53.0f) } },
          { { lowCut, 35.0f, 0.0f, 0.7f }, { bell, 300.0f, -1.5f, 0.9f }, { highShelf, 10000.0f, 2.0f, 0.7f } } },
        { "Garage Shuffle", "BEATS", "Skippy 2-step swing, a bouncy 1/8 pump, a little chorus",
          { { Params::swing, swingAt (65.5f) }, { humanize, 0.15f }, { pump, 0.35f }, { pumpMode, eighth },
            { wobble, 0.22f }, { wobbleMode, chorus }, { space, 0.22f }, { spaceTime, t8T }, { spaceMode, pingPong } },
          { { lowCut, 40.0f, 0.0f, 0.7f }, { highShelf, 8000.0f, 1.5f, 0.7f } } },
        { "Jersey Bounce", "BEATS", "Club bounce: triplet stutters on every hit, a punchy eighth pump",
          { { timeSlot, slotOf ("STUTTER 3") }, { timeMix, 0.55f }, { pump, 0.3f }, { pumpMode, eighth },
            { drive, 0.15f }, { dirtMode, warm }, { space, 0.1f }, { spaceTime, t8T }, { spaceMode, pingPong } },
          { { lowShelf, 70.0f, 3.0f, 0.7f }, { highShelf, 7000.0f, 2.0f, 0.7f } } },
        { "Drill Glide", "BEATS", "Dark and heavy: every beat drags behind itself, a deep low end, a closed top",
          { { timeSlot, slotOf ("LAZY") }, { timeMix, 0.6f }, { drive, 0.25f }, { dirtMode, warm }, { filter, -0.18f },
            { space, 0.15f }, { spaceTime, t4 }, { spaceMode, stereo }, { humanize, 0.05f } },
          { { lowShelf, 55.0f, 4.5f, 0.7f }, { bell, 500.0f, -2.5f, 1.0f } } },
        { "Dilla Drunk", "BEATS", "Wonky and alive: late eighths, loose timing, a warm, soft edge",
          { { timeSlot, slotOf ("SHUFFLE") }, { timeMix, 0.5f }, { Params::swing, swingAt (62.0f) }, { humanize, 0.45f },
            { drive, 0.25f }, { dirtMode, warm }, { filter, -0.15f } },
          { { lowShelf, 100.0f, 2.0f, 0.7f }, { highShelf, 7500.0f, -2.5f, 0.7f } } },
        { "Phonk Cowbell", "BEATS", "Memphis haze: fuzz, a wobbly tape, a cowbell-bright mid, echoing beats",
          { { drive, 0.35f }, { dirtMode, fuzz }, { wobble, 0.2f }, { wobbleMode, tape }, { space, 0.25f }, { spaceTime, t4 }, { spaceMode, pingPong }, { filter, -0.1f },
            { timeSlot, slotOf ("ECHO BEAT") }, { timeMix, 0.3f } },
          { { lowShelf, 60.0f, 3.0f, 0.7f }, { bell, 800.0f, 3.0f, 1.2f } } },

        // --- VOCALS -----------------------------------------------------------------
        // Vocal chops: the lows cleaned up, the mud out, presence and air on top.
        { "Vox Chop Shine", "VOCALS", "Clean, bright vocal chops: no mud, a forward middle, silky air, a soft chorus",
          { { space, 0.25f }, { wobble, 0.15f }, { wobbleMode, chorus } },
          { { lowCut, 140.0f, 0.0f, 0.7f }, { bell, 300.0f, -2.5f, 1.0f }, { bell, 3200.0f, 2.0f, 0.9f }, { highShelf, 10000.0f, 3.0f, 0.7f } } },
        { "Future Bass Vox", "VOCALS", "Pumping, wide and glittering: the chop that answers the drop",
          { { pump, 0.55f }, { pumpMode, eighth }, { wobble, 0.3f }, { wobbleMode, chorus }, { space, 0.4f },
            { grainMix, 0.25f }, { grainShimmer, 0.35f }, { grainSize, 140.0f }, { grainSpread, 1.0f } },
          { { lowCut, 150.0f, 0.0f, 0.7f }, { highShelf, 9000.0f, 3.0f, 0.7f } } },
        { "Chipmunk Choir", "VOCALS", "The voice an octave up in soft grains, layered into a little choir",
          { { grainMix, 0.55f }, { grainPitch, 12.0f }, { grainSize, 120.0f }, { grainDensity, 40.0f }, { grainShape, soft },
            { grainSpread, 1.0f }, { grainSpray, 0.25f }, { grainDetune, 0.35f }, { space, 0.35f }, { spaceTime, t4 }, { spaceMode, stereo } },
          { { lowCut, 200.0f, 0.0f, 0.7f }, { bell, 4000.0f, 1.5f, 0.8f } } },
        { "Vocal Stutter", "VOCALS", "Sixteenth stutters on every syllable: the classic edit",
          { { timeSlot, slotOf ("STUTTER 16") }, { timeMix, 0.6f }, { space, 0.2f }, { spaceTime, t16 }, { spaceMode, pingPong } },
          { { lowCut, 120.0f, 0.0f, 0.7f }, { highShelf, 8000.0f, 2.0f, 0.7f } } },
        { "Reverse Vox Swell", "VOCALS", "Every beat of the voice backwards, swelling into the next",
          { { timeSlot, slotOf ("REV SWELL") }, { timeMix, 0.8f }, { space, 0.55f }, { spaceTime, t4 }, { spaceMode, stereo }, { grainMix, 0.2f }, { grainReverse, 0.6f }, { grainSize, 200.0f } },
          { { lowCut, 150.0f, 0.0f, 0.7f } } },
        { "Lo-Fi Vox", "VOCALS", "A voice from an old tape: warm, worn and band-limited",
          { { filter, -0.3f }, { drive, 0.3f }, { dirtMode, warm }, { wobble, 0.3f }, { wobbleMode, tape }, { space, 0.15f }, { spaceTime, t8 }, { spaceMode, stereo } },
          { { lowCut, 250.0f, 0.0f, 0.8f }, { highCut, 6000.0f, 0.0f, 0.8f }, { bell, 1200.0f, 2.0f, 0.9f } } },
        { "Vox Choir Pad", "VOCALS", "The voice spread into a soft chord pad in the sample's key",
          { { grainMix, 0.9f }, { grainSize, 300.0f }, { grainDensity, 40.0f }, { grainScale, chord }, { grainPitchRnd, 12.0f },
            { grainShape, soft }, { grainDrift, 0.3f }, { grainSpread, 1.0f }, { grainDetune, 0.4f }, { grainHaze, 0.35f }, { space, 0.5f }, { spaceTime, t4D }, { spaceMode, stereo }, { filter, -0.1f } },
          { { lowCut, 120.0f, 0.0f, 0.7f } } },
        { "Glitch Vox", "VOCALS", "Syllables that repeat, stick and flip, sprinkled with tiny grains",
          { { timeSlot, slotOf ("GLITCH") }, { timeMix, 0.7f }, { grainMix, 0.3f }, { grainSize, 30.0f }, { grainDensity, 90.0f },
            { grainSpray, 0.5f }, { drive, 0.2f }, { dirtMode, crush } },
          { { lowCut, 150.0f, 0.0f, 0.7f } } },
        { "Vox Delay Throw", "VOCALS", "Long ping-pong echoes thrown after every phrase, thin and bright",
          { { space, 0.7f }, { spaceTime, t4D }, { spaceMode, pingPong }, { filter, 0.1f } },
          { { lowCut, 180.0f, 0.0f, 0.7f }, { bell, 2500.0f, 2.0f, 0.9f } } },
        { "Telephone Vox", "VOCALS", "The voice down a phone line, a little broken, echoing",
          { { drive, 0.2f }, { dirtMode, fuzz }, { space, 0.3f }, { spaceTime, t4 }, { spaceMode, pingPong } },
          { { lowCut, 600.0f, 0.0f, 0.9f }, { highCut, 3000.0f, 0.0f, 0.9f }, { bell, 1500.0f, 7.0f, 0.7f } } },

        // --- LO-FI ------------------------------------------------------------------
        { "Cassette Nostalgia", "LO-FI", "Worn tape: wow and flutter, soft saturation, the highs rolled off",
          { { wobble, 0.45f }, { wobbleMode, tape }, { drive, 0.3f }, { dirtMode, warm }, { filter, -0.38f },
            { resonance, 0.1f }, { humanize, 0.2f }, { space, 0.18f }, { spaceTime, t8 }, { spaceMode, stereo } },
          { { lowShelf, 150.0f, 2.0f, 0.7f } } },
        { "Vinyl Sunday", "LO-FI", "A warm record: a gentle filter, a slowly swaying platter, lazy swing",
          { { drive, 0.22f }, { dirtMode, warm }, { filter, -0.3f }, { wobble, 0.3f }, { wobbleMode, tape },
            { Params::swing, swingAt (57.5f) } },
          { { lowCut, 50.0f, 0.0f, 0.7f }, { bell, 220.0f, 2.0f, 0.8f } } },
        { "Late Night Radio", "LO-FI", "Small and far away: a band-limited AM radio that fades in and out",
          { { drive, 0.18f }, { dirtMode, fuzz }, { wobble, 0.12f }, { wobbleMode, tape },
            { timeSlot, slotOf ("BREATHE") }, { timeMix, 0.35f }, { timeLength, twoBars } },
          { { lowCut, 350.0f, 0.0f, 0.9f }, { highCut, 3200.0f, 0.0f, 0.9f }, { bell, 1300.0f, 8.0f, 0.7f } } },
        { "8-Bit Arcade", "LO-FI", "A crunchy console: bit-crushed, thin and bright, glitching on thirty-seconds",
          { { drive, 0.62f }, { dirtMode, crush }, { space, 0.08f }, { spaceTime, t16 }, { spaceMode, pingPong }, { timeSlot, slotOf ("STUTTER 32") }, { timeMix, 0.35f },
            { timeLength, halfBar } },
          { { lowCut, 140.0f, 0.0f, 0.7f }, { highShelf, 6000.0f, 2.0f, 0.7f } } },
        { "Warped Tape", "LO-FI", "A melted cassette: the pitch sways deeply, soft and dark",
          { { wobble, 0.6f }, { wobbleMode, tape }, { drive, 0.3f }, { dirtMode, warm },
            { filter, -0.35f }, { space, 0.15f }, { spaceTime, t4 }, { spaceMode, stereo } },
          { { lowCut, 45.0f, 0.0f, 0.7f }, { lowShelf, 180.0f, 2.0f, 0.7f } } },
        { "Dusty Sampler", "LO-FI", "An old 12-bit sampler: gritty, punchy lows, no top end",
          { { drive, 0.35f }, { dirtMode, crush }, { filter, -0.28f }, { Params::swing, swingAt (56.0f) }, { humanize, 0.15f } },
          { { lowShelf, 90.0f, 3.0f, 0.7f }, { highCut, 11000.0f, 0.0f, 0.7f } } },
        { "Bedroom Chill", "LO-FI", "Soft and sleepy: a dark filter, a gentle chorus, dragging beats, a little sparkle",
          { { filter, -0.4f }, { space, 0.3f }, { spaceTime, t4D }, { spaceMode, stereo }, { wobble, 0.25f }, { wobbleMode, chorus }, { grainMix, 0.15f },
            { grainShimmer, 0.3f }, { grainSize, 180.0f }, { timeSlot, slotOf ("LAZY") }, { timeMix, 0.4f },
            { Params::swing, swingAt (60.0f) } },
          { { lowShelf, 120.0f, 1.5f, 0.7f } } },

        // --- TEXTURES -----------------------------------------------------------------
        { "Frozen Cathedral", "TEXTURES", "The chop held still and smeared into a huge shimmering cloud that swells",
          { { grainMix, 0.9f }, { grainSize, 380.0f }, { grainDensity, 32.0f }, { grainScan, 0.04f }, { grainSpray, 0.5f },
            { grainDrift, 0.55f }, { grainShimmer, 0.45f }, { grainSpread, 1.0f }, { grainReverse, 0.2f }, { grainShape, soft },
            { grainHaze, 0.6f }, { grainDetune, 0.3f },
            { space, 0.5f }, { spaceTime, t2 }, { spaceMode, stereo }, { filter, -0.1f }, { timeSlot, slotOf ("REV SWELL") }, { timeMix, 0.3f }, { timeLength, twoBars } },
          {} },
        { "Glass Shimmer", "TEXTURES", "Sparkling octave grains over the original, bright and wide",
          { { grainMix, 0.55f }, { grainSize, 110.0f }, { grainDensity, 48.0f }, { grainShimmer, 0.85f },
            { grainScale, octaves }, { grainPitchRnd, 12.0f }, { grainSpread, 0.85f }, { grainDetune, 0.2f }, { grainHaze, 0.35f }, { space, 0.35f } },
          { { lowCut, 80.0f, 0.0f, 0.7f }, { highShelf, 8000.0f, 3.0f, 0.7f } } },
        { "Cloud Choir", "TEXTURES", "A soft, breathing pad from any sample: long grains, slow drift, chorus",
          { { grainMix, 0.85f }, { grainSize, 260.0f }, { grainDensity, 40.0f }, { grainShape, soft },
            { grainDrift, 0.4f }, { grainSpray, 0.35f }, { grainSpread, 1.0f }, { grainDetune, 0.45f }, { grainHaze, 0.45f }, { wobble, 0.3f }, { wobbleMode, chorus },
            { space, 0.4f }, { spaceTime, t4D }, { spaceMode, stereo }, { filter, -0.18f }, { timeSlot, slotOf ("BREATHE") }, { timeMix, 0.4f }, { timeLength, twoBars } },
          {} },
        { "Rewind Dust", "TEXTURES", "Grains and beats running backwards with a warm, dusty edge",
          { { grainMix, 0.7f }, { grainSize, 95.0f }, { grainDensity, 26.0f }, { grainReverse, 0.85f }, { grainSpray, 0.3f },
            { drive, 0.2f }, { dirtMode, warm }, { filter, -0.22f }, { space, 0.2f }, { timeSlot, slotOf ("REVERSE") }, { timeMix, 0.45f } },
          {} },
        { "Glitch Swarm", "TEXTURES", "Tiny scattered grains, random pitches, a glitching time grid",
          { { grainMix, 0.75f }, { grainSize, 22.0f }, { grainDensity, 110.0f }, { grainSpray, 0.8f }, { grainPitchRnd, 7.0f },
            { grainReverse, 0.4f }, { grainShape, hard }, { drive, 0.3f }, { dirtMode, crush },
            { timeSlot, slotOf ("GLITCH") }, { timeMix, 0.6f } },
          {} },
        { "Deep Drone", "TEXTURES", "One moment stretched into a dark, endless drone an octave down, breathing slowly",
          { { grainMix, 1.0f }, { grainSize, 480.0f }, { grainDensity, 14.0f }, { grainScan, 0.0f }, { grainPosition, 0.3f },
            { grainDrift, 0.3f }, { grainPitch, -12.0f }, { grainSpread, 0.7f }, { grainShape, soft }, { grainDetune, 0.25f }, { grainHaze, 0.5f },
            { filter, -0.4f }, { space, 0.45f }, { spaceTime, t2 }, { spaceMode, stereo },
            { timeSlot, slotOf ("BREATHE") }, { timeMix, 0.5f }, { timeLength, twoBars } },
          { { lowCut, 40.0f, 0.0f, 0.7f } } },
        { "Melted Clock", "TEXTURES", "Time falls apart: slowed-down grains, a slowed-down bar, drifting echoes",
          { { grainMix, 0.6f }, { grainSize, 200.0f }, { grainDensity, 20.0f }, { grainScan, 0.5f }, { grainPitch, -5.0f },
            { grainDrift, 0.5f }, { grainGlide, -0.25f }, { grainHaze, 0.3f }, { wobble, 0.5f }, { wobbleMode, tape }, { space, 0.45f }, { spaceTime, t4T }, { spaceMode, pingPong }, { filter, -0.25f },
            { timeSlot, slotOf ("SLOWMO") }, { timeMix, 0.7f } },
          {} },
        { "Chord Bloom", "TEXTURES", "Every grain picks a chord tone: a harmonious, glowing cloud in the sample's key",
          { { grainMix, 0.8f }, { grainSize, 220.0f }, { grainDensity, 36.0f }, { grainPitchRnd, 12.0f }, { grainScale, chord },
            { grainShape, soft }, { grainSpray, 0.4f }, { grainDrift, 0.3f }, { grainSpread, 1.0f }, { grainDetune, 0.3f }, { grainHaze, 0.35f },
            { space, 0.4f }, { spaceTime, t4 }, { spaceMode, stereo }, { filter, -0.12f } },
          {} },
        { "Stutter Grid", "TEXTURES", "Hard grains locked to 1/16, jumping octaves: a glitchy rhythm from any chop",
          { { grainMix, 0.85f }, { grainSync, 1.0f }, { grainDensity, 28.0f }, { grainSize, 70.0f }, { grainShape, hard },
            { grainPitchRnd, 12.0f }, { grainScale, octaves }, { grainReverse, 0.3f }, { grainSpray, 0.25f }, { grainSpread, 0.8f },
            { space, 0.15f }, { spaceTime, t16 }, { spaceMode, pingPong } },
          {} },
        { "Frozen Harp", "TEXTURES", "One frozen moment plucked over and over in fifths, like a harp made of the sample",
          { { grainMix, 0.9f }, { grainFreeze, 1.0f }, { grainShape, pluck }, { grainSize, 160.0f }, { grainDensity, 18.0f },
            { grainPitchRnd, 12.0f }, { grainScale, fifths }, { grainPosition, 0.25f }, { grainSpread, 0.9f }, { space, 0.35f } },
          {} },
        { "Granular Rain", "TEXTURES", "Hundreds of tiny plucked drops in the chord of the sample, falling all around",
          { { grainMix, 0.75f }, { grainSize, 45.0f }, { grainDensity, 90.0f }, { grainSpray, 0.9f }, { grainShape, pluck },
            { grainPitchRnd, 12.0f }, { grainScale, chord }, { grainSpread, 1.0f }, { grainShimmer, 0.2f }, { grainHaze, 0.3f }, { space, 0.35f }, { spaceTime, t8 }, { spaceMode, pingPong } },
          { { lowCut, 120.0f, 0.0f, 0.7f }, { highShelf, 7000.0f, 2.0f, 0.7f } } },
        { "Octave Ghosts", "TEXTURES", "Backwards grains an octave up, swelling in like reversed cymbals",
          { { grainMix, 0.7f }, { grainPitch, 12.0f }, { grainShimmer, 0.5f }, { grainReverse, 0.6f }, { grainSize, 240.0f },
            { grainDensity, 24.0f }, { grainShape, soft }, { grainSpread, 1.0f }, { grainHaze, 0.5f }, { space, 0.5f }, { spaceTime, t4D }, { spaceMode, stereo },
            { timeSlot, slotOf ("REV SWELL") }, { timeMix, 0.6f } },
          { { lowCut, 150.0f, 0.0f, 0.7f } } },
        { "Fifth Bells", "TEXTURES", "Plucked grains on the beat in fifths: a music box built from your chop",
          { { grainMix, 0.8f }, { grainSync, 1.0f }, { grainDensity, 12.0f }, { grainSize, 140.0f }, { grainShape, pluck },
            { grainPitchRnd, 12.0f }, { grainScale, fifths }, { grainSpread, 0.9f }, { space, 0.4f } },
          { { lowCut, 180.0f, 0.0f, 0.7f }, { highShelf, 6000.0f, 3.0f, 0.7f } } },

        { "Haze Garden", "TEXTURES", "A lush, slowly blooming pad: detuned chord grains melting into a long haze",
          { { grainMix, 0.85f }, { grainSize, 320.0f }, { grainDensity, 30.0f }, { grainShape, soft }, { grainPitchRnd, 12.0f },
            { grainScale, chord }, { grainDetune, 0.6f }, { grainHaze, 0.85f }, { grainDrift, 0.35f }, { grainSpray, 0.3f },
            { grainSpread, 1.0f }, { filter, -0.15f } },
          { { lowCut, 90.0f, 0.0f, 0.7f } } },
        { "Laser Rain", "TEXTURES", "Short plucked grains bending upwards like laser shots, scattered across the room",
          { { grainMix, 0.75f }, { grainSize, 90.0f }, { grainDensity, 55.0f }, { grainShape, pluck }, { grainGlide, 0.85f },
            { grainSpray, 0.7f }, { grainSpread, 1.0f }, { grainHaze, 0.3f }, { space, 0.25f }, { spaceTime, t16 }, { spaceMode, pingPong } },
          { { lowCut, 150.0f, 0.0f, 0.7f }, { highShelf, 7000.0f, 2.0f, 0.7f } } },
        { "Sinking Ship", "TEXTURES", "Long grains slowly bending down into the dark, groaning and drifting",
          { { grainMix, 0.8f }, { grainSize, 420.0f }, { grainDensity, 18.0f }, { grainScan, 0.25f }, { grainGlide, -0.6f },
            { grainShape, soft }, { grainDrift, 0.4f }, { grainHaze, 0.55f }, { grainSpread, 0.8f }, { filter, -0.35f },
            { wobble, 0.25f }, { wobbleMode, tape } },
          { { lowCut, 35.0f, 0.0f, 0.7f } } },
        { "Unison Wall", "TEXTURES", "A huge wall of detuned grains: wide, beating and thick like a stack of synths",
          { { grainMix, 0.7f }, { grainSize, 160.0f }, { grainDensity, 80.0f }, { grainDetune, 1.0f }, { grainSpread, 1.0f },
            { grainShape, soft }, { grainSpray, 0.15f }, { grainHaze, 0.25f }, { drive, 0.15f }, { dirtMode, warm } },
          { { lowCut, 60.0f, 0.0f, 0.7f }, { highShelf, 9000.0f, 1.5f, 0.7f } } },

        // --- MOTION -------------------------------------------------------------------
        { "Sidechain Sunrise", "MOTION", "Big, bright pumping with chorus and space: instant euphoria",
          { { pump, 0.85f }, { pumpMode, quarter }, { wobble, 0.3f }, { wobbleMode, chorus }, { space, 0.35f } },
          { { lowCut, 40.0f, 0.0f, 0.7f }, { highShelf, 9000.0f, 2.5f, 0.7f } } },
        { "Stutter Gate", "MOTION", "Hard 1/16 chopping over every chop: classic gate rhythms",
          { { timeSlot, slotOf ("GATE 16") }, { drive, 0.15f }, { dirtMode, warm }, { space, 0.15f }, { spaceTime, t16 }, { spaceMode, pingPong } },
          {} },
        { "Seasick", "MOTION", "Everything sways: deep tape wobble, a slow wow and a lazy 1/8 pump",
          { { wobble, 0.9f }, { wobbleMode, tape }, { pump, 0.3f }, { pumpMode, eighth }, { space, 0.3f }, { spaceTime, t4T }, { spaceMode, stereo }, { filter, -0.15f } },
          {} },
        { "Trance Gate", "MOTION", "Accented sixteenths, each with its own snap: the gate of the big rooms",
          { { timeSlot, slotOf ("TRANCE 16") }, { space, 0.35f }, { wobble, 0.2f }, { wobbleMode, chorus } },
          { { lowCut, 60.0f, 0.0f, 0.7f }, { highShelf, 8000.0f, 2.0f, 0.7f } } },
        { "Dotted Dream", "MOTION", "A dotted-eighth gate against the beat, echoing across the stereo field",
          { { timeSlot, slotOf ("GATE DOT") }, { timeMix, 0.85f }, { space, 0.5f }, { filter, 0.08f }, { wobble, 0.15f }, { wobbleMode, chorus } },
          {} },
        { "Tremolo Surf", "MOTION", "A surf-guitar tremolo on every eighth, chorus and a warm drive",
          { { timeSlot, slotOf ("TREMOLO") }, { wobble, 0.35f }, { wobbleMode, chorus }, { space, 0.3f }, { spaceTime, t8 }, { spaceMode, stereo }, { drive, 0.15f }, { dirtMode, warm } },
          { { bell, 1200.0f, 2.0f, 0.8f } } },
        { "Offbeat Skank", "MOTION", "Only the off-beats, thin and echoing: a reggae skank from any chop",
          { { timeSlot, slotOf ("OFFBEAT") }, { filter, 0.25f }, { space, 0.45f }, { wobble, 0.1f }, { wobbleMode, tape } },
          { { bell, 2000.0f, 2.5f, 1.0f } } },
        { "Sidechain Eighths", "MOTION", "Ducking on every eighth: a breathless, driving pulse",
          { { pump, 0.7f }, { pumpMode, eighth }, { space, 0.25f }, { spaceTime, t8 }, { spaceMode, pingPong }, { drive, 0.1f }, { dirtMode, warm } },
          { { lowShelf, 80.0f, 2.0f, 0.7f } } },

        { "Heartbeat", "MOTION", "A double pulse on every beat, quiet in between: tense and alive",
          { { timeSlot, slotOf ("HEARTBEAT") }, { timeMix, 0.9f }, { filter, -0.12f }, { space, 0.15f }, { spaceTime, t4 }, { spaceMode, stereo } },
          { { lowShelf, 90.0f, 3.0f, 0.7f } } },
        { "Stairway", "MOTION", "Every beat climbs a staircase of level, pushing into the next",
          { { timeSlot, slotOf ("STAIRS") }, { space, 0.3f }, { wobble, 0.15f }, { wobbleMode, chorus } },
          { { lowCut, 50.0f, 0.0f, 0.7f } } },

        // --- TIME ---------------------------------------------------------------------
        { "Half-Time Heaven", "TIME", "Everything at half speed, an octave down, wide and drifting",
          { { timeSlot, slotOf ("HALF TIME") }, { space, 0.3f }, { spaceTime, t4 }, { spaceMode, stereo }, { filter, -0.2f }, { grainMix, 0.2f }, { grainDrift, 0.3f },
            { grainSize, 220.0f }, { grainShape, soft } },
          { { lowCut, 35.0f, 0.0f, 0.7f } } },
        { "Tape Stop Drop", "TIME", "Every second bar the tape grinds to a halt: the classic drop",
          { { timeSlot, slotOf ("TAPE STOP") }, { timeLength, twoBars }, { drive, 0.2f }, { dirtMode, warm } },
          {} },
        { "Turntablist", "TIME", "Baby scratches on every beat, a dusty crunch, a tight swing",
          { { timeSlot, slotOf ("SCRATCH") }, { drive, 0.25f }, { dirtMode, warm }, { Params::swing, swingAt (56.0f) } },
          { { lowCut, 60.0f, 0.0f, 0.7f }, { bell, 1500.0f, 2.0f, 0.9f } } },
        { "Rewind Selector", "TIME", "The selector pulls it back: two bars play, the last beat rewinds them",
          { { timeSlot, slotOf ("REWIND") }, { timeLength, twoBars }, { space, 0.25f }, { spaceTime, t4 }, { spaceMode, pingPong } },
          {} },
        { "Build-Up Machine", "TIME", "A drum roll that rises over two bars, thinning out towards the drop",
          { { timeSlot, slotOf ("BUILD UP") }, { timeLength, twoBars }, { filter, 0.18f }, { space, 0.3f }, { spaceTime, t8 }, { spaceMode, pingPong } },
          {} },
        { "Glitch Cutter", "TIME", "Sixteenths that repeat, stick and flip, with a crushed edge",
          { { timeSlot, slotOf ("GLITCH") }, { drive, 0.3f }, { dirtMode, crush }, { grainMix, 0.2f }, { grainSize, 30.0f },
            { grainDensity, 80.0f }, { grainSpray, 0.6f } },
          {} },
        { "Polyrhythm Lab", "TIME", "Dotted eighths against the beat: three over four, every bar",
          { { timeSlot, slotOf ("DOTTED") }, { timeMix, 0.8f }, { pump, 0.3f }, { pumpMode, eighth }, { space, 0.2f } },
          {} },
        { "Brake Lights", "TIME", "A turntable brake at the end of every bar, warped and warm",
          { { timeSlot, slotOf ("BRAKE") }, { wobble, 0.3f }, { wobbleMode, tape }, { filter, -0.15f }, { drive, 0.15f }, { dirtMode, warm } },
          {} },
        { "Stop And Go", "TIME", "The bar slows to a standstill and spins back up again",
          { { timeSlot, slotOf ("STOP START") }, { timeLength, twoBars }, { space, 0.2f } },
          {} },
        { "Reverse Psych", "TIME", "The second half of the bar backwards, in a swirl of chorus and echoes",
          { { timeSlot, slotOf ("REV HALF") }, { wobble, 0.4f }, { wobbleMode, chorus }, { space, 0.45f }, { spaceTime, t4T }, { spaceMode, pingPong }, { filter, -0.1f } },
          {} },

        { "Fly By", "TIME", "Every bar flies past like a car: higher and louder coming, lower and fading going",
          { { timeSlot, slotOf ("FLYBY") }, { space, 0.3f }, { spaceTime, t8 }, { spaceMode, pingPong }, { wobble, 0.1f }, { wobbleMode, chorus } },
          {} },
        { "Arp Climber", "TIME", "The bar climbs a little arpeggio, a step higher every beat, echoing",
          { { timeSlot, slotOf ("ARP UP") }, { space, 0.4f }, { filter, -0.1f } },
          {} },
        { "Octave Jumper", "TIME", "Eighths an octave down, sixteenths an octave up: the tune jumps around itself",
          { { timeSlot, slotOf ("OCTAVES") }, { timeMix, 0.8f }, { drive, 0.15f }, { dirtMode, warm }, { space, 0.2f }, { spaceTime, t16 }, { spaceMode, pingPong } },
          {} },
        { "Gate Ramp Build", "TIME", "A gate that speeds up over two bars and opens into the drop",
          { { timeSlot, slotOf ("GATE RAMP") }, { timeLength, twoBars }, { filter, 0.12f }, { space, 0.25f }, { spaceTime, t8 }, { spaceMode, pingPong } },
          { { lowCut, 80.0f, 0.0f, 0.7f } } },

        // --- SPACE --------------------------------------------------------------------
        { "Dub Echo Chamber", "SPACE", "Long, dark ping-pong echoes in a warm dub haze",
          { { space, 0.78f }, { spaceTime, t4D }, { spaceMode, pingPong }, { filter, -0.32f }, { drive, 0.22f }, { dirtMode, warm }, { wobble, 0.15f }, { wobbleMode, tape } },
          { { lowCut, 90.0f, 0.0f, 0.7f }, { bell, 900.0f, -2.0f, 0.8f } } },
        { "Wide Air", "SPACE", "Open and expensive: an airy top, gentle chorus width, a touch of echo",
          { { space, 0.38f }, { spaceTime, t8 }, { spaceMode, stereo }, { wobble, 0.22f }, { wobbleMode, chorus } },
          { { lowCut, 35.0f, 0.0f, 0.7f }, { highShelf, 11000.0f, 3.5f, 0.7f }, { bell, 350.0f, -1.5f, 0.8f } } },
        { "Echo Cascade", "SPACE", "Each bar answers itself three times, quieter and further away",
          { { timeSlot, slotOf ("ECHO") }, { timeMix, 0.7f }, { space, 0.55f }, { spaceTime, t4 }, { spaceMode, pingPong }, { filter, -0.12f } },
          { { lowCut, 70.0f, 0.0f, 0.7f } } },
        { "Cathedral Wash", "SPACE", "A huge, washed-out room: long grains, swelling beats, endless echoes",
          { { space, 0.9f }, { spaceTime, t2 }, { spaceMode, stereo }, { grainMix, 0.4f }, { grainSize, 400.0f }, { grainDrift, 0.4f }, { grainShape, soft }, { grainSpread, 1.0f }, { grainHaze, 0.7f },
            { filter, -0.2f }, { timeSlot, slotOf ("SWELL") }, { timeMix, 0.4f } },
          { { lowCut, 100.0f, 0.0f, 0.7f } } },
        { "Telephone Dub", "SPACE", "A thin, distant line echoing into the void",
          { { space, 0.6f }, { spaceTime, t4 }, { spaceMode, pingPong }, { drive, 0.3f }, { dirtMode, fuzz } },
          { { lowCut, 500.0f, 0.0f, 0.9f }, { highCut, 3000.0f, 0.0f, 0.9f }, { bell, 1300.0f, 9.0f, 0.6f } } },

        // --- DESTROY ------------------------------------------------------------------
        { "Blown Speaker", "DESTROY", "Driven into a broken speaker: fuzz, a resonant cut, a fat low bump",
          { { drive, 0.78f }, { dirtMode, fuzz }, { filter, -0.32f }, { resonance, 0.45f }, { space, 0.1f } },
          { { bell, 120.0f, 5.0f, 0.9f } } },
        { "Bitcrushed Ghost", "DESTROY", "Crushed, haunted and half backwards: grains, echoes, a thin top",
          { { drive, 0.8f }, { dirtMode, crush }, { grainMix, 0.35f }, { grainSize, 60.0f }, { grainReverse, 0.55f },
            { grainSpray, 0.3f }, { space, 0.42f }, { spaceTime, t8T }, { spaceMode, pingPong }, { filter, 0.22f } },
          {} },
        { "Crushed Stutter", "DESTROY", "Bit-crushed thirty-second stutters tearing through every chop",
          { { drive, 0.7f }, { dirtMode, crush }, { timeSlot, slotOf ("STUTTER 32") }, { timeMix, 0.6f }, { filter, 0.15f } },
          {} },
        { "Fuzz Riot", "DESTROY", "Full fuzz, a screaming resonant filter, sixteenth pumping",
          { { drive, 0.9f }, { dirtMode, fuzz }, { pump, 0.4f }, { pumpMode, sixteenth }, { filter, -0.3f }, { resonance, 0.6f } },
          { { notch, 450.0f, 0.0f, 2.0f }, { bell, 2000.0f, 4.0f, 1.0f } } },
        { "Broken Robot", "DESTROY", "A malfunctioning machine: tiny hard grains in octaves, glitching, crushed",
          { { drive, 0.6f }, { dirtMode, crush }, { grainMix, 0.6f }, { grainSize, 15.0f }, { grainDensity, 120.0f },
            { grainShape, hard }, { grainPitchRnd, 12.0f }, { grainScale, octaves }, { timeSlot, slotOf ("GLITCH") }, { timeMix, 0.5f } },
          {} },
        { "Warp Core", "DESTROY", "A reactor about to blow: crushed, detuned grains screaming upwards into a haze",
          { { drive, 0.55f }, { dirtMode, crush }, { grainMix, 0.7f }, { grainSize, 70.0f }, { grainDensity, 70.0f },
            { grainGlide, 1.0f }, { grainDetune, 0.8f }, { grainHaze, 0.5f }, { grainSpray, 0.5f }, { grainShape, hard },
            { filter, 0.1f }, { resonance, 0.4f } },
          {} },
    };
    return all;
}

juce::StringArray categories()
{
    return { "BEATS", "VOCALS", "LO-FI", "TEXTURES", "MOTION", "TIME", "SPACE", "DESTROY" };
}

bool inScope (const juce::String& id)
{
    static const juce::StringArray ids {
        space, spaceTime, spaceMode, drive, dirtMode, wobble, wobbleMode, filter, resonance, pump, pumpMode,
        grainMix, grainSize, grainDensity, grainSpray, grainPitchRnd, grainSpread, grainScan, grainReverse,
        grainPosition, grainPitch, grainDrift, grainShimmer, grainShape, grainScale, grainFreeze, grainSync,
        grainGlide, grainDetune, grainHaze,
        timeSlot, timeMix, timeLength,
        master, Params::swing, humanize
    };
    return ids.contains (id) || (id.startsWith ("eq") && id.containsChar ('_'));
}

juce::ValueTree toSound (const Preset& preset)
{
    juce::ValueTree state ("SOUND");
    auto add = [&state] (const juce::String& id, float value)
    {
        juce::ValueTree param ("PARAM");
        param.setProperty ("id", id, nullptr);
        param.setProperty ("value", value, nullptr);
        state.appendChild (param, nullptr);
    };

    for (const auto& [id, value] : preset.values)
        add (id, value);

    // Level matching: the master level that brings each preset to about the same loudness
    // (measured on a drum loop), so switching presets does not jump in volume.
    static const std::map<juce::String, float> levels {
        // VOCALS: set for a voice, not measured on the drum loop (its low cut takes the
        // kick away, which a voice does not have): pump and grain pads up a little.
        { "Vox Chop Shine", -1.0f }, { "Future Bass Vox", 2.0f }, { "Chipmunk Choir", 1.0f }, { "Reverse Vox Swell", 2.0f },
        { "Vox Choir Pad", 3.0f }, { "Telephone Vox", -1.0f },
        { "Boom Bap Dust", 1.0f },
        { "Trap Knock", -5.0f },
        { "House Pump", 3.0f },
        { "Garage Shuffle", 6.0f },
        { "Jersey Bounce", -1.0f },
        { "Drill Glide", -4.5f },
        { "Dilla Drunk", -5.0f },
        { "Phonk Cowbell", -8.5f },
        { "Cassette Nostalgia", -5.5f },
        { "Vinyl Sunday", 2.5f },
        { "Late Night Radio", 6.0f },
        { "8-Bit Arcade", 6.0f },
        { "Warped Tape", -3.0f },
        { "Dusty Sampler", -4.0f },
        { "Bedroom Chill", 2.5f },
        { "Frozen Cathedral", 6.0f },
        { "Glass Shimmer", 5.5f },
        { "Cloud Choir", 6.0f },
        { "Rewind Dust", 4.5f },
        { "Glitch Swarm", -1.0f },
        { "Deep Drone", 6.0f },
        { "Melted Clock", 4.5f },
        { "Chord Bloom", 5.5f },
        { "Stutter Grid", 1.0f },
        { "Frozen Harp", 6.0f },
        { "Granular Rain", 6.0f },
        { "Octave Ghosts", 6.0f },
        { "Fifth Bells", 1.5f },
        { "Sidechain Sunrise", 6.0f },
        { "Stutter Gate", -0.5f },
        { "Seasick", 3.0f },
        { "Trance Gate", 6.0f },
        { "Dotted Dream", 2.0f },
        { "Tremolo Surf", 4.5f },
        { "Offbeat Skank", 6.0f },
        { "Sidechain Eighths", 5.5f },
        { "Half-Time Heaven", 0.0f },
        { "Tape Stop Drop", -2.5f },
        { "Turntablist", 1.0f },
        { "Rewind Selector", -2.0f },
        { "Build-Up Machine", 2.5f },
        { "Glitch Cutter", -3.0f },
        { "Polyrhythm Lab", -1.5f },
        { "Brake Lights", -2.0f },
        { "Stop And Go", 1.5f },
        { "Reverse Psych", 6.0f },
        { "Dub Echo Chamber", 5.0f },
        { "Wide Air", 3.0f },
        { "Echo Cascade", 5.5f },
        { "Cathedral Wash", 6.0f },
        { "Telephone Dub", 5.0f },
        { "Blown Speaker", -10.5f },
        { "Bitcrushed Ghost", -2.0f },
        { "Crushed Stutter", -6.5f },
        { "Fuzz Riot", -6.5f },
        { "Broken Robot", 0.0f },
    };
    if (const auto it = levels.find (preset.name); it != levels.end() && it->second != 0.0f)
        add (master, it->second);

    for (size_t band = 0; band < preset.eq.size() && (int) band < eqBands; ++band)
    {
        const auto& p = preset.eq[band];
        add (eqParamID ((int) band, EqField::on), 1.0f);
        add (eqParamID ((int) band, EqField::type), (float) p.type);
        add (eqParamID ((int) band, EqField::freq), p.freq);
        add (eqParamID ((int) band, EqField::gain), p.gainDb);
        add (eqParamID ((int) band, EqField::q), p.q);
    }

    state.setProperty ("name", preset.name, nullptr);
    return state;
}

juce::ValueTree randomSound (uint32_t seed, juce::String& description)
{
    juce::Random rng ((juce::int64) seed);
    auto between = [&rng] (float lo, float hi) { return lo + (hi - lo) * rng.nextFloat(); };
    auto logBetween = [&rng] (float lo, float hi) { return lo * std::pow (hi / lo, rng.nextFloat()); };
    auto chance = [&rng] (float p) { return rng.nextFloat() < p; };

    // Which effects: each by its own odds, then two to five of them.
    enum Effect { delayFx, distortionFx, filterFx, wobbleFx, pumpFx, grainFx, timeFx, eqFx, numEffects };
    const float odds[] = { 0.55f, 0.45f, 0.45f, 0.4f, 0.35f, 0.3f, 0.35f, 0.5f };
    std::array<bool, numEffects> on {};
    int count = 0;
    for (int e = 0; e < numEffects; ++e)
        count += (on[(size_t) e] = chance (odds[e])) ? 1 : 0;
    while (count < 2)
        if (const auto e = rng.nextInt (numEffects); ! on[(size_t) e]) { on[(size_t) e] = true; ++count; }
    while (count > 5)
        if (const auto e = rng.nextInt (numEffects); on[(size_t) e]) { on[(size_t) e] = false; --count; }

    juce::ValueTree state ("SOUND");
    auto add = [&state] (const juce::String& id, float value)
    {
        juce::ValueTree param ("PARAM");
        param.setProperty ("id", id, nullptr);
        param.setProperty ("value", value, nullptr);
        state.appendChild (param, nullptr);
    };
    juce::StringArray rolled;
    float level = 0.0f; // dB, against what the effects add or take

    if (on[delayFx])
    {
        static const int times[] = { 0, 1, 2, 2, 3, 3, 4, 5, 5, 6 }; // the 1/8s and 1/4s most
        const auto time = times[rng.nextInt (10)];
        add (space, between (0.18f, 0.6f));
        add (spaceTime, (float) time);
        add (spaceMode, chance (0.7f) ? 0.0f : 1.0f);
        rolled.add (juce::String ("Delay ") + PingPongDelay::Time::names[time]);
    }
    if (on[distortionFx])
    {
        const auto mode = rng.nextInt (3);
        const auto amount = mode == 0 ? between (0.2f, 0.65f) : (mode == 1 ? between (0.15f, 0.55f) : between (0.3f, 0.75f));
        add (drive, amount);
        add (dirtMode, (float) mode);
        level -= amount * (mode == 1 ? 9.0f : 6.0f);
        rolled.add (mode == 0 ? "Warm" : (mode == 1 ? "Fuzz" : "Crush"));
    }
    if (on[filterFx])
    {
        const auto lowPass = chance (0.65f);
        add (filter, lowPass ? -between (0.1f, 0.45f) : between (0.08f, 0.35f));
        add (resonance, between (0.05f, 0.55f));
        rolled.add (lowPass ? "Low-pass" : "High-pass");
        level += lowPass ? 1.5f : 2.5f;
    }
    if (on[wobbleFx])
    {
        add (wobble, between (0.1f, 0.45f));
        add (wobbleMode, chance (0.5f) ? tape : chorus);
        rolled.add ("Wobble");
    }
    if (on[pumpFx])
    {
        const auto amount = between (0.25f, 0.7f);
        add (pump, amount);
        add (pumpMode, (float) rng.nextInt (3));
        level += amount * 4.0f;
        rolled.add ("Pump");
    }
    if (on[grainFx])
    {
        add (grainMix, between (0.2f, 0.65f));
        add (grainSize, logBetween (20.0f, 400.0f));
        add (grainDensity, logBetween (8.0f, 80.0f));
        add (grainSpray, between (0.0f, 0.6f));
        add (grainSpread, between (0.3f, 1.0f));
        add (grainShape, chance (0.4f) ? soft : (chance (0.5f) ? hard : pluck));
        add (grainDrift, between (0.0f, 0.5f));
        if (chance (0.3f)) { add (grainPitchRnd, 12.0f); add (grainScale, chance (0.5f) ? octaves : fifths); }
        if (chance (0.3f)) add (grainReverse, between (0.2f, 0.6f));
        if (chance (0.2f)) add (grainShimmer, between (0.15f, 0.45f));
        if (chance (0.15f)) add (grainGlide, (chance (0.5f) ? 1.0f : -1.0f) * between (0.3f, 1.0f));
        if (chance (0.25f)) add (grainDetune, between (0.2f, 0.8f));
        if (chance (0.35f)) add (grainHaze, between (0.2f, 0.8f));
        level += 1.0f;
        rolled.add ("Grains");
    }
    if (on[timeFx])
    {
        const auto slot = rng.nextInt (TimeFactory::numSlots);
        add (timeSlot, (float) (slot + 1));
        add (timeMix, between (0.35f, 0.8f));
        add (timeLength, (float) rng.nextInt (3));
        rolled.add (juce::String ("Time ") + TimeFactory::name (slot));
    }
    if (on[eqFx])
    {
        int band = 0;
        auto point = [&] (int type, float freq, float gain, float q)
        {
            add (eqParamID (band, EqField::on), 1.0f);
            add (eqParamID (band, EqField::type), (float) type);
            add (eqParamID (band, EqField::freq), freq);
            add (eqParamID (band, EqField::gain), gain);
            add (eqParamID (band, EqField::q), q);
            ++band;
        };
        if (chance (0.5f)) point (lowCut, logBetween (30.0f, 160.0f), 0.0f, 0.7f);
        if (chance (0.5f)) point (lowShelf, logBetween (60.0f, 160.0f), between (-3.0f, 4.0f), 0.7f);
        if (chance (0.6f)) point (bell, logBetween (200.0f, 3000.0f), between (-3.5f, 3.5f), between (0.7f, 1.5f));
        if (band == 0 || chance (0.5f)) point (highShelf, logBetween (6000.0f, 12000.0f), between (-3.0f, 3.5f), 0.7f);
        rolled.add ("EQ");
    }

    if (std::abs (level) >= 0.5f)
        add (master, juce::jlimit (-8.0f, 4.0f, level));

    state.setProperty ("name", "Random", nullptr);
    description = rolled.joinIntoString (", ");
    return state;
}

} // namespace kerf::Factory
