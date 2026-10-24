#include "MusicalAnalysis.h"

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <numeric>

namespace kerf
{

juce::String KeyAnalyzer::noteName (int pc)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return pc >= 0 && pc < 12 ? names[pc] : "?";
}

juce::String KeyEstimate::toString() const
{
    if (tonic < 0)
        return "--";
    return KeyAnalyzer::noteName (tonic) + (minor ? " min" : " maj");
}

//==============================================================================
NameTags readNameTags (const juce::String& fileName)
{
    NameTags tags;
    auto name = fileName.replace (juce::CharPointer_UTF8 ("\xe2\x99\xaf"), "#").replace (juce::CharPointer_UTF8 ("\xe2\x99\xad"), "b");
    for (const auto* extension : { ".wav", ".wave", ".aif", ".aiff", ".flac", ".ogg", ".mp3" })
        if (name.endsWithIgnoreCase (extension))
            name = name.dropLastCharacters ((int) std::strlen (extension));

    // Words: runs of letters or of digits (and '#': C#), with what stands before and after them.
    struct Word { juce::String text; juce::juce_wchar before = 0, after = 0; };
    std::vector<Word> words;
    auto isWordChar = [] (juce::juce_wchar c) { return (c < 128 && juce::CharacterFunctions::isLetterOrDigit (c)) || c == '#'; };
    for (int i = 0; i < name.length();)
    {
        if (! isWordChar (name[i]))
        {
            ++i;
            continue;
        }
        const auto start = i;
        const auto digits = juce::CharacterFunctions::isDigit (name[i]); // "125bpm": two words
        while (i < name.length() && isWordChar (name[i]) && juce::CharacterFunctions::isDigit (name[i]) == digits)
            ++i;
        words.push_back ({ name.substring (start, i), start > 0 ? name[start - 1] : 0, i < name.length() ? name[i] : 0 });
    }

    // The key: "Gmin", "A#min", "Ebm", "F#minor", or "F" + "Min" / "minor" / "m".
    auto noteOf = [] (const juce::String& text, int& length)
    {
        length = 0;
        if (text.isEmpty() || text[0] < 'A' || text[0] > 'G')
            return -1;
        static const int natural[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        auto pc = natural[text[0] - 'A'];
        length = 1;
        if (text.length() > 1 && text[1] == '#') { ++pc; length = 2; }
        else if (text.length() > 1 && text[1] == 'b') { --pc; length = 2; }
        return (pc + 12) % 12;
    };
    auto modeOf = [] (const juce::String& text)
    {
        if (text.equalsIgnoreCase ("min") || text.equalsIgnoreCase ("minor") || text == "m" || text.equalsIgnoreCase ("moll"))
            return 1;
        if (text.equalsIgnoreCase ("maj") || text.equalsIgnoreCase ("major") || text.equalsIgnoreCase ("dur"))
            return 0;
        return -1;
    };
    int keyWord = -1, keyLength = 1; // "F" "Min": two words
    bool keyWasBareM = false;
    for (int w = 0; w < (int) words.size(); ++w)
    {
        int length = 0;
        const auto& text = words[(size_t) w].text;
        const auto pc = noteOf (text, length);
        if (pc < 0)
            continue;
        const auto rest = text.substring (length);
        auto mode = rest.isNotEmpty() ? modeOf (rest) : -1;
        if (rest.isEmpty() && w + 1 < (int) words.size())
            mode = modeOf (words[(size_t) w + 1].text);
        // "Am" alone could be a word ("I Am"): a key only among the tags ("Am 90", "140bpm F#m")
        // and when nothing better is named.
        const auto bareM = rest == "m";
        auto isTag = [&words] (int at)
        {
            return at >= 0 && at < (int) words.size()
                && (words[(size_t) at].text.containsOnly ("0123456789") || words[(size_t) at].text.equalsIgnoreCase ("bpm") || words[(size_t) at].text.equalsIgnoreCase ("key"));
        };
        if (mode < 0 || (bareM && ((keyWord >= 0 && ! keyWasBareM) || ! (isTag (w - 1) || isTag (w + 1)))))
            continue;
        tags.tonic = pc;
        tags.minor = mode == 1;
        keyWord = w;
        keyLength = rest.isEmpty() ? 2 : 1;
        keyWasBareM = bareM;
    }

    // The tempo: "125bpm" / "bpm 125" first, then "(90)", then a number next to the key,
    // then the plain numbers from 60 to 200, the last first (not "Vol.3", "v2", "No. 096").
    std::vector<std::pair<int, double>> found; // rank, bpm
    const auto numberAfterKey = keyWord >= 0 && keyWord + keyLength < (int) words.size()
                                && words[(size_t) (keyWord + keyLength)].text.containsOnly ("0123456789");
    for (int w = 0; w < (int) words.size(); ++w)
    {
        const auto& word = words[(size_t) w];
        if (! word.text.containsOnly ("0123456789") || word.text.length() > 3 || word.text[0] == '0')
            continue;
        const auto value = word.text.getIntValue();
        if (value < 60 || value > 200 || word.before == '.' || word.after == '.')
            continue;
        const auto previous = w > 0 ? words[(size_t) w - 1].text.toLowerCase() : juce::String();
        if (previous == "v" || previous == "vol" || previous == "no" || previous == "nr" || previous == "take" || previous == "part")
            continue;
        const auto next = w + 1 < (int) words.size() ? words[(size_t) w + 1].text.toLowerCase() : juce::String();
        int rank = 1000 - w; // the plain numbers: the last first
        if (next == "bpm" || previous == "bpm")
            rank = 0;
        else if (word.before == '(' && word.after == ')')
            rank = 1;
        else if (keyWord >= 0 && (w == keyWord + keyLength || (w == keyWord - 1 && ! numberAfterKey)))
            rank = 2; // next to the key (with numbers on both sides, the one after it: the one before is an index)
        found.push_back ({ rank, (double) value });
    }
    std::sort (found.begin(), found.end());
    for (const auto& f : found)
        tags.tempos.push_back (f.second);
    tags.sure = (int) std::count_if (found.begin(), found.end(), [] (const std::pair<int, double>& f) { return f.first <= 2; });
    return tags;
}

//==============================================================================
TempoEstimate TempoAnalyzer::estimate (const OnsetAnalysis& a, int64_t numFrames, const juce::AudioBuffer<float>* audio,
                                       const NameTags& named)
{
    // 1. Coarse tempo: autocorrelation of the onset detection function (ODF) at one, two
    //    and four beats, with a broad preference for common tempos.
    // 2. Precise tempo: the beat grid (period and phase) that collects the most onset
    //    energy over the whole sample, searched in 0.01 BPM steps around the coarse value.
    // 3. Octave: tempo is ambiguous by a factor of two. Musical evidence decides: beats that
    //    alternate strong / weak at a fast tempo mean half the tempo; a slow tempo with busy
    //    subdivisions (hi-hats) means double the tempo.
    // 4. Loop length: samples cut to a whole number of bars give the exact tempo. Checked on
    //    the full length and on the length without leading / trailing silence, and only
    //    accepted when it agrees closely with the measured tempo.
    // 5. Values within a hair of a whole BPM are snapped to it (most music is made at whole
    //    tempos).
    TempoEstimate result;
    // The ODF slightly compressed: a few big isolated spikes (vinyl crackle) count less
    // against many regular onsets.
    std::vector<float> odfStore (a.odf.begin(), a.odf.end());
    for (auto& v : odfStore)
        v = std::pow (std::max (0.0f, v), 0.7f);
    const auto& odf = odfStore;
    const auto fps = a.odfFrameRate();

    auto namedOnly = [&named, &result]
    {
        if (named.sure != 1)
            return result; // (one marked tempo: that one; of several, the audio would have to decide)
        result.bpm = named.tempos.front();
        result.fromName = true;
        result.confidence = 1.0f;
        return result;
    };
    if (odf.size() < 64 || fps <= 0.0)
        return namedOnly();

    const auto size = odf.size();

    // Smoothed ODF for grid fitting, mean-removed copy for the autocorrelation.
    std::vector<float> s (size), x (size);
    const auto mean = std::accumulate (odf.begin(), odf.end(), 0.0f) / (float) size;
    for (size_t i = 0; i < size; ++i)
    {
        const auto prev = odf[i > 0 ? i - 1 : i], next = odf[i + 1 < size ? i + 1 : i];
        s[i] = 0.25f * prev + 0.5f * odf[i] + 0.25f * next;
        x[i] = odf[i] - mean;
    }

    auto acfAt = [&] (double lagFrames)
    {
        const auto l0 = (size_t) lagFrames;
        const auto frac = (float) (lagFrames - (double) l0);
        double s0 = 0.0, s1 = 0.0;
        for (size_t i = 0; i + l0 + 1 < size; ++i)
        {
            s0 += (double) x[i] * x[i + l0];
            s1 += (double) x[i] * x[i + l0 + 1];
        }
        return (float) ((1.0 - frac) * s0 + frac * s1) / (float) size;
    };

    // The rhythm is measured above the ODF's floor (its median): a noise floor or crackle
    // fills the gaps between the beats and would otherwise look like busy subdivisions
    // (worn records were read at double tempo).
    {
        std::vector<float> sorted (s);
        const auto k = size / 2;
        std::nth_element (sorted.begin(), sorted.begin() + (ptrdiff_t) k, sorted.end());
        const auto floorLevel = sorted[k];
        for (auto& v : s)
            v = std::max (0.0f, v - floorLevel);
    }

    auto at = [&s, size] (double pos)
    {
        if (pos < 0.0 || pos >= (double) (size - 1))
            return 0.0f;
        const auto i = (size_t) pos;
        const auto f = (float) (pos - (double) i);
        return s[i] + (s[i + 1] - s[i]) * f;
    };

    // Mean onset energy on a grid (period, phase) shifted by `offset` periods.
    auto gridMean = [&] (double period, double phase, double offset, int parity = -1)
    {
        double sum = 0.0;
        int count = 0, k = 0;
        for (double t = phase + offset * period; t < (double) size; t += period, ++k)
            if (parity < 0 || (k % 2) == parity)
            {
                sum += at (t);
                ++count;
            }
        return count > 0 ? sum / count : 0.0;
    };

    // Best phase for a period (and its on-beat mean).
    auto fitGrid = [&] (double period, double& phaseOut)
    {
        double best = -1.0;
        for (double phase = 0.0; phase < period; phase += 0.5)
            if (const auto m = gridMean (period, phase, 0.0); m > best)
            {
                best = m;
                phaseOut = phase;
            }
        return best;
    };

    // Fine search around a tempo (+/- span), 0.01 BPM steps after a 0.05 BPM pass.
    auto refine = [&] (double bpm, double span, double& phaseOut)
    {
        double bestBpm = bpm, best = -1.0, phase = 0.0;
        for (double b = bpm * (1.0 - span); b <= bpm * (1.0 + span); b += 0.05)
            if (const auto m = fitGrid (fps * 60.0 / b, phase); m > best) { best = m; bestBpm = b; }
        const auto centre = bestBpm;
        for (double b = centre - 0.06; b <= centre + 0.06; b += 0.01)
            if (const auto m = fitGrid (fps * 60.0 / b, phase); m > best) { best = m; bestBpm = b; }
        fitGrid (fps * 60.0 / bestBpm, phaseOut);
        return bestBpm;
    };

    // --- 1. Coarse --------------------------------------------------------------------------
    double coarse = 0.0;
    float bestScore = -1.0e9f;
    std::vector<float> scores;
    for (double bpm = 50.0; bpm <= 220.0; bpm += 0.5)
    {
        const auto lag = fps * 60.0 / bpm;
        // The bar (4 beats) counts fully: steady 16th hi-hats make every multiple of a 16th
        // look periodic, but only the real beat repeats the kick / snare pattern every bar.
        const auto raw = acfAt (lag) + 0.5f * acfAt (lag * 2.0) + acfAt (lag * 4.0);
        const auto prior = (float) std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.9, 2.0));
        const auto score = raw * (0.5f + 0.5f * prior);
        scores.push_back (score);
        if (score > bestScore)
        {
            bestScore = score;
            coarse = bpm;
        }
    }

    if (bestScore <= 0.0f)
        return namedOnly();

    // Confidence from how much the best peak stands out.
    const auto scoreMean = std::accumulate (scores.begin(), scores.end(), 0.0f) / (float) scores.size();
    float variance = 0.0f;
    for (auto v : scores)
        variance += (v - scoreMean) * (v - scoreMean);
    const auto stdDev = std::sqrt (variance / (float) scores.size());
    const auto z = stdDev > 0.0f ? (bestScore - scoreMean) / stdDev : 0.0f;
    result.confidence = juce::jlimit (0.0f, 1.0f, (z - 1.5f) / 2.5f);

    // --- 2. Precise -------------------------------------------------------------------------
    double phase = 0.0;
    auto bpm = refine (coarse, 0.02, phase);

    // --- 3. Loop length ---------------------------------------------------------------------
    std::vector<double> lengths { (double) numFrames };
    if (audio != nullptr && audio->getNumSamples() > 0)
    {
        // Without leading / trailing silence (-45 dB below the peak, 10 ms blocks).
        const auto peak = audio->getMagnitude (0, audio->getNumSamples());
        const auto threshold = peak * 0.0056f;
        const auto block = std::max (1, (int) (a.sampleRate * 0.01));
        int first = -1, last = -1;
        for (int i = 0; i + block <= audio->getNumSamples(); i += block)
            if (audio->getMagnitude (i, block) > threshold)
            {
                if (first < 0) first = i;
                last = i + block;
            }
        if (first >= 0)
        {
            lengths.push_back ((double) (numFrames - first));
            lengths.push_back ((double) (last - first));
            lengths.push_back ((double) last);
        }
    }

    // The loop tempo (a whole number of beats over one of the lengths) closest to `around`.
    auto loopMatch = [&] (double around, double tolerance)
    {
        double best = 0.0, bestError = tolerance;
        for (const auto length : lengths)
        {
            const auto seconds = length / a.sampleRate;
            if (seconds <= 0.0)
                continue;
            for (int beats : { 4, 8, 12, 16, 24, 32, 48, 64, 2, 6 })
            {
                const auto loopBpm = beats * 60.0 / seconds;
                if (loopBpm < 60.0 || loopBpm > 190.0)
                    continue;
                if (const auto error = std::abs (loopBpm - around) / around; error < bestError)
                {
                    bestError = error;
                    best = loopBpm;
                }
            }
        }
        return best;
    };

    // Within 0.4 % of the measured tempo (looser lets a ringing tail pass for a bar line).
    constexpr double loopTolerance = 0.004;
    auto loopBpm = loopMatch (bpm, loopTolerance);

    // Syncopation (dotted rhythms, busy hi-hats) can make the measured tempo a 3:4 or 4:5
    // relative of the real one. For a loop, the real tempo also makes the length a whole
    // number of bars: take it if the beat grid fits (almost) as well there.
    if (loopBpm <= 0.0)
    {
        double ignored = 0.0;
        const auto fitHere = fitGrid (fps * 60.0 / bpm, ignored);
        for (double ratio : { 0.75, 4.0 / 3.0, 0.8, 1.25, 2.0 / 3.0, 1.5 })
        {
            const auto candidate = loopMatch (bpm * ratio, 0.012);
            if (candidate <= 0.0)
                continue;
            if (fitGrid (fps * 60.0 / candidate, ignored) >= 0.9 * fitHere)
            {
                loopBpm = candidate;
                break;
            }
        }
    }

    // Exact loops: a sample exported as a whole number of beats at a whole (or half) BPM is
    // that long to the sample - rare by chance. When the rhythm did not match one of the
    // loop's tempos (pads, a sparse bass, an unsure grid), the exact tempo whose beat grid
    // fits best (the usual tempos, around 110, a little ahead) decides, unless the grid
    // clearly fits the measured tempo better.
    auto prior = [] (double b) { return std::exp (-0.5 * std::pow (std::log2 (b / 110.0) / 0.6, 2.0)); };
    if (loopBpm <= 0.0)
    {
        const auto seconds = (double) numFrames / a.sampleRate;
        double ignored = 0.0, exactScore = 0.0, exactBpm = 0.0;
        for (int beats : { 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256 })
        {
            const auto whole = std::round (beats * 60.0 / seconds * 2.0) / 2.0; // a whole or half BPM
            if (whole < 60.0 || whole > 200.0)
                continue;
            if (std::abs (beats * 60.0 / whole * a.sampleRate - (double) numFrames) > std::max (2.0, 0.0005 * a.sampleRate))
                continue;
            const auto score = (fitGrid (fps * 60.0 / whole, ignored) + 1.0e-6) * prior (whole) * (whole == std::round (whole) ? 1.0 : 0.6);
            if (score > exactScore)
            {
                exactScore = score;
                exactBpm = whole;
            }
        }
        if (exactBpm > 0.0 && fitGrid (fps * 60.0 / exactBpm, ignored) >= 0.5 * fitGrid (fps * 60.0 / bpm, ignored))
            loopBpm = exactBpm;
    }

    if (loopBpm > 0.0)
    {
        bpm = loopBpm;
        result.fromLoopLength = true;
        result.confidence = std::min (1.0f, result.confidence + 0.35f);
    }

    // --- 4. Octave --------------------------------------------------------------------------
    // Tempo is ambiguous by a factor of two; the rhythm between the beats decides.
    //  - From 140 BPM up with nothing at all between the beats: these "beats" are the 8th notes
    //    of half the tempo (e.g. boom-bap at 85 heard as 170).
    //  - Below 90 BPM with busy 16th notes: a half-time feel over fast hi-hats (trap, drum and
    //    bass), counted at double the tempo. 8th notes alone (soul, boom-bap) do not count.
    {
        const auto period = fps * 60.0 / bpm;
        const auto onBeat = gridMean (period, phase, 0.0);
        const auto half = gridMean (period, phase, 0.5);
        const auto sixteenths = 0.5 * (gridMean (period, phase, 0.25) + gridMean (period, phase, 0.75));

        auto target = bpm;
        if (bpm > 185.0 || (bpm >= 139.5 && half < 0.15 * onBeat))
            target = bpm * 0.5;
        else if (bpm < 62.0 || (bpm < 90.0 && sixteenths > 0.45 * onBeat))
            target = bpm * 2.0;
        else if (bpm < 80.0)
        {
            // Slow, with little between the beats: these "beats" may be the half notes of
            // twice the tempo (a sparse bass at 125 read as 62.5). The grid that fits better,
            // with the usual tempos ahead, decides.
            double ignored = 0.0;
            if (fitGrid (period * 0.5, ignored) * prior (bpm * 2.0) > onBeat * prior (bpm))
                target = bpm * 2.0;
        }

        if (target != bpm)
        {
            // A loop stays a loop at double / half the tempo (exact); otherwise measure again.
            const auto exact = result.fromLoopLength ? loopMatch (target, loopTolerance) : 0.0;
            bpm = exact > 0.0 ? exact : refine (target, 0.01, phase);
            result.fromLoopLength = exact > 0.0;
        }
    }

    // --- The file name ---------------------------------------------------------------------
    // It names the tempo the loop was made at. A tempo marked as one ("128bpm", "(90)", next
    // to the key) is the answer; of several such (an index next to the key, too), the one
    // the loop's length (a whole number of beats) and the measured tempo agree with, the usual
    // tempos a little ahead. A bare number only when the measured tempo or the loop's length
    // agrees: a one-shot's index is no tempo.
    const auto& tempos = named.tempos;
    auto agreement = [&] (double candidate) // 2: the measured tempo, 1: double or half of it, 0: neither
    {
        if (std::abs (candidate / bpm - 1.0) < 0.03)
            return 2;
        return std::abs (candidate * 2.0 / bpm - 1.0) < 0.03 || std::abs (candidate * 0.5 / bpm - 1.0) < 0.03 ? 1 : 0;
    };
    auto fitsLength = [&] (double candidate)
    {
        const auto beats = (double) numFrames / a.sampleRate * candidate / 60.0;
        return beats >= 3.5 && std::abs (beats - std::round (beats)) < 0.02;
    };
    double chosen = 0.0;
    if (named.sure > 0)
    {
        double best = -1.0;
        for (int i = 0; i < named.sure; ++i)
        {
            const auto t = tempos[(size_t) i];
            const auto score = (fitsLength (t) ? 2.0 : 0.0) + agreement (t) + prior (t) - 0.01 * i;
            if (score > best)
            {
                best = score;
                chosen = t;
            }
        }
    }
    else
    {
        for (const auto& t : tempos)
            if (chosen == 0.0 && (agreement (t) > 0 || fitsLength (t)))
                chosen = t;
    }
    if (chosen > 0.0)
    {
        bpm = chosen;
        result.fromName = true;
        result.fromLoopLength = false;
        result.confidence = 1.0f;
    }

    // --- 5. Whole tempos --------------------------------------------------------------------
    if (std::abs (bpm - std::round (bpm)) < 0.12)
        bpm = std::round (bpm);
    else if (std::abs (bpm * 2.0 - std::round (bpm * 2.0)) < 0.1)
        bpm = std::round (bpm * 2.0) / 2.0;
    else
        bpm = std::round (bpm * 100.0) / 100.0;
    result.bpm = bpm;

    // Beat phase for the final tempo; ODF frame -> sample frame.
    fitGrid (fps * 60.0 / bpm, phase);
    result.firstBeat = std::max<int64_t> (0, (int64_t) (phase * a.hopSize));
    if (result.fromLoopLength && phase * a.hopSize < 0.06 * a.sampleRate)
        result.firstBeat = 0; // loops normally start on the beat

    return result;
}

//==============================================================================
namespace
{
    struct Peak { double freq; double mag; };

    /** Median of a small range (n <= 32). */
    float medianOf (const float* values, int n)
    {
        std::array<float, 32> tmp {};
        n = std::min (n, 32);
        std::copy_n (values, n, tmp.begin());
        std::nth_element (tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
        return tmp[(size_t) (n / 2)];
    }

    // Key profiles. Major: Albrecht and Shanahan (2013), better than Krumhansl-Kessler at
    // telling a key from its dominant. Minor: theirs moved towards natural minor (the flat
    // 6th and 7th up, the raised 7th down), as minor keys are used in pop, hip-hop and
    // electronic music (i - bVII - bVI); with the classical profile those loops were read
    // as their relative major.
    constexpr double majorProfile[] = { 0.238, 0.006, 0.111, 0.006, 0.137, 0.094, 0.016, 0.214, 0.009, 0.080, 0.008, 0.081 };
    constexpr double minorProfile[] = { 0.220, 0.006, 0.104, 0.123, 0.019, 0.103, 0.012, 0.214, 0.085, 0.022, 0.095, 0.025 };

    // A bass line in a key: mostly the tonic, then the fifth and the fourth.
    constexpr double bassProfile[] = { 0.36, 0.01, 0.05, 0.04, 0.04, 0.13, 0.01, 0.17, 0.05, 0.06, 0.05, 0.03 };
}

KeyEstimate KeyAnalyzer::estimate (const juce::AudioBuffer<float>& audio, double sampleRate, const std::function<bool()>& shouldExit)
{
    // 1. Magnitude spectrogram (8192-point FFT, 90 s at most, spread over the file).
    // 2. Harmonic / percussive separation by median filtering (drums are short in time and
    //    wide in frequency; notes are long and narrow); only the harmonic part is used.
    // 3. Spectral peaks with parabolic interpolation. The tuning offset of the recording is
    //    estimated from them, so detuned samples still land on the right notes.
    // 4. Peaks that are overtones (3rd, 5th, 6th harmonic) of a lower, comparable peak count
    //    much less: otherwise every note also votes for its fifth and the key drifts to the
    //    dominant.
    // 5. A pitch-class profile is correlated with major / minor key profiles in all 12
    //    transpositions, and a bass profile (the loop's opening counting extra) with a bass
    //    line in that key.
    KeyEstimate result;
    const auto mono = TransientDetector::mixToMono (audio);

    constexpr int fftOrder = 13;
    constexpr int fftSize = 1 << fftOrder;
    const int hop = fftSize / 2;

    const auto n = (int64_t) mono.size();
    if (n < fftSize)
        return result;

    const auto totalFrames = (int) ((n - fftSize) / hop) + 1;
    const auto maxFrames = (int) (90.0 * sampleRate / hop);
    const auto frameStep = std::max (1, totalFrames / std::max (1, maxFrames));

    const auto binHz = sampleRate / fftSize;
    const auto maxBin = std::min (fftSize / 2 - 1, (int) (4500.0 / binHz));
    const auto minBin = std::max (2, (int) (28.0 / binHz));
    const auto numBins = maxBin + 1;

    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> frame ((size_t) fftSize * 2);

    std::vector<std::vector<float>> spec;
    for (int f = 0; f < totalFrames; f += frameStep)
    {
        if (shouldExit && shouldExit())
            return {};
        std::fill (frame.begin(), frame.end(), 0.0f);
        std::copy_n (mono.begin() + (ptrdiff_t) f * hop, fftSize, frame.begin());
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data(), true);
        spec.emplace_back (frame.begin(), frame.begin() + numBins);
    }

    const auto numFrames = (int) spec.size();
    if (numFrames == 0)
        return result;

    // --- Harmonic part (median filters: 9 frames in time, 17 bins in frequency) -----------
    std::vector<std::vector<float>> harmonic (spec.size(), std::vector<float> ((size_t) numBins, 0.0f));
    {
        std::array<float, 32> column {};
        for (int f = 0; f < numFrames; ++f)
        {
            if (shouldExit && shouldExit())
                return {};
            const auto& m = spec[(size_t) f];
            for (int k = minBin; k <= maxBin; ++k)
            {
                int c = 0;
                for (int g = std::max (0, f - 4); g <= std::min (numFrames - 1, f + 4); ++g)
                    column[(size_t) c++] = spec[(size_t) g][(size_t) k];
                const auto h = medianOf (column.data(), c);
                const auto lo = std::max (minBin, k - 8), hi = std::min (maxBin, k + 8);
                const auto p = medianOf (m.data() + lo, hi - lo + 1);
                const auto mask = h * h / (h * h + p * p + 1.0e-12f);
                harmonic[(size_t) f][(size_t) k] = m[(size_t) k] * mask;
            }
        }
    }

    // --- Peaks and tuning ----------------------------------------------------------------
    std::vector<std::vector<Peak>> framePeaks ((size_t) numFrames);
    double tuneSin = 0.0, tuneCos = 0.0;
    for (int f = 0; f < numFrames; ++f)
    {
        const auto& m = harmonic[(size_t) f];
        const auto frameMax = *std::max_element (m.begin(), m.end());
        if (frameMax <= 1.0e-6f)
            continue;

        for (int k = minBin + 1; k < maxBin; ++k)
        {
            if (! (m[(size_t) k] > m[(size_t) k - 1] && m[(size_t) k] >= m[(size_t) k + 1] && m[(size_t) k] > frameMax * 0.02f))
                continue;

            // Parabolic interpolation on log magnitudes.
            const auto a = std::log (m[(size_t) k - 1] + 1.0e-9), b = std::log (m[(size_t) k] + 1.0e-9), c = std::log (m[(size_t) k + 1] + 1.0e-9);
            const auto denom = a - 2.0 * b + c;
            const auto delta = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
            const auto freq = (k + delta) * binHz;
            const auto mag = std::exp (b - 0.25 * (a - c) * delta);
            framePeaks[(size_t) f].push_back ({ freq, mag });

            if (freq > 80.0 && freq < 2000.0)
            {
                const auto cents = 1200.0 * std::log2 (freq / 440.0);
                const auto angle = juce::MathConstants<double>::twoPi * cents / 100.0;
                tuneSin += mag * std::sin (angle);
                tuneCos += mag * std::cos (angle);
            }
        }
    }

    // Tuning offset of the recording from A440, in cents (-50 .. +50).
    const auto tuningCents = (tuneSin == 0.0 && tuneCos == 0.0) ? 0.0 : std::atan2 (tuneSin, tuneCos) * 100.0 / juce::MathConstants<double>::twoPi;

    // --- Pitch-class profiles ------------------------------------------------------------
    // The loop's opening counts extra in the bass profile: loops are cut on the downbeat,
    // and the bass of the first chord is usually the tonic. This is what tells a minor key
    // from its relative major (G minor from B flat major: the same notes), which the
    // overall note profile cannot. The opening is the first 12 % of the sample (at most 3 s,
    // so a long file's intro does not decide).
    std::array<double, 12> chroma {}, bass {};
    const auto openingFrames = std::max (1, std::min ((int) std::lround (numFrames * 0.12), (int) (3.0 * sampleRate / (hop * frameStep))));
    constexpr double openingBassWeight = 6.0;

    for (int f = 0; f < numFrames; ++f)
    {
        const auto& peaks = framePeaks[(size_t) f];
        std::array<double, 12> c {}, b {};
        double sumC = 0.0, sumB = 0.0;

        // Overtones: going up from the lowest peak, each peak's own (residual) strength
        // predicts its 3rd, 5th and 6th harmonics (about 1/h as strong). Only what is left
        // after removing that prediction counts as a note. A real chord tone that happens to
        // coincide with the bass's overtone (the third over a root in the bass) survives;
        // a bare overtone does not.
        // Octave overtones are included too, so they cannot act as fundamentals themselves,
        // and only the strongest prediction counts (several notes can share an overtone).
        std::vector<double> residual (peaks.size(), 0.0), predicted (peaks.size(), 0.0);
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            residual[i] = std::max (0.0, peaks[i].mag - predicted[i]); // all predictors are lower: done
            if (residual[i] <= 0.0)
                continue;
            for (int h = 2; h <= 6; ++h)
            {
                const auto target = peaks[i].freq * h;
                for (size_t j = i + 1; j < peaks.size(); ++j)
                    if (std::abs (1200.0 * std::log2 (peaks[j].freq / target)) < 35.0)
                        predicted[j] = std::max (predicted[j], residual[i] / h);
            }
        }

        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const auto& p = peaks[i];
            if (residual[i] <= 0.0)
                continue;

            const auto midi = 69.0 + 12.0 * std::log2 (p.freq / 440.0) - tuningCents / 100.0;
            const auto pc = ((int) std::lround (midi) % 12 + 12) % 12;
            const auto v = std::sqrt (residual[i]);

            if (p.freq < 180.0)
            {
                b[(size_t) pc] += v;
                sumB += v;
            }
            if (p.freq >= 60.0)
            {
                c[(size_t) pc] += v;
                sumC += v;
            }
        }

        // Per-frame normalisation so loud passages do not dominate.
        const auto bassWeight = f < openingFrames ? openingBassWeight : 1.0;
        for (int i = 0; i < 12; ++i)
        {
            if (sumC > 0.0) chroma[(size_t) i] += c[(size_t) i] / sumC;
            if (sumB > 0.0) bass[(size_t) i] += bassWeight * b[(size_t) i] / sumB;
        }
    }

    auto normalise = [] (std::array<double, 12>& v)
    {
        const auto sum = std::accumulate (v.begin(), v.end(), 0.0);
        if (sum > 0.0)
            for (auto& x : v) x /= sum;
    };
    normalise (chroma);
    normalise (bass);

    auto pearson = [] (const std::array<double, 12>& a, const double* ref, int rotation)
    {
        double ma = 0, mb = 0;
        for (int i = 0; i < 12; ++i) { ma += a[(size_t) i]; mb += ref[i]; }
        ma /= 12.0; mb /= 12.0;
        double num = 0, da = 0, db = 0;
        for (int i = 0; i < 12; ++i)
        {
            const auto x = a[(size_t) ((i + rotation) % 12)] - ma;
            const auto y = ref[i] - mb;
            num += x * y; da += x * x; db += y * y;
        }
        return da > 0 && db > 0 ? num / std::sqrt (da * db) : 0.0;
    };

    // A key's score: how well the notes fit its profile, plus (half as much) how well the
    // bass fits a bass line in that key (mostly on the tonic, then the fifth and fourth).
    // The flatter the notes (noisy, distorted, atonal stabs), the more the bass decides.
    double spread = 0.0;
    for (const auto v : chroma)
        spread += (v - 1.0 / 12.0) * (v - 1.0 / 12.0);
    spread = std::sqrt (spread / 12.0);
    const auto bassScoreWeight = 0.5 * juce::jlimit (1.0, 3.0, 0.04 / std::max (1.0e-4, spread));
    double best = -2.0, second = -2.0, bestNotes = 0.0;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (int isMinor = 0; isMinor < 2; ++isMinor)
        {
            const auto notes = pearson (chroma, isMinor ? minorProfile : majorProfile, tonic);
            const auto r = notes + bassScoreWeight * pearson (bass, bassProfile, tonic);
            if (r > best)
            {
                second = best;
                best = r;
                bestNotes = notes;
                result.tonic = tonic;
                result.minor = isMinor != 0;
            }
            else if (r > second)
            {
                second = r;
            }
        }

    // Major or minor: without a clear major third, minor (a bare root and fifth - a techno
    // bass, a power chord - is heard as minor in most of the music made with samples).
    auto noMajorThird = [&chroma] (int tonic)
    {
        return chroma[(size_t) ((tonic + 4) % 12)] < std::max (0.06, 1.5 * chroma[(size_t) ((tonic + 3) % 12)]);
    };
    if (result.tonic >= 0 && ! result.minor && noMajorThird (result.tonic))
    {
        result.minor = true;
        bestNotes = pearson (chroma, minorProfile, result.tonic);
    }

    // Confidence: a clear winner whose notes fit its key well.
    result.confidence = (float) juce::jlimit (0.0, 1.0, (best - second) * 4.0 * juce::jlimit (0.0, 1.0, (bestNotes - 0.3) / 0.5));
    if (bestNotes < 0.3)
        result.tonic = -1; // nothing tonal (e.g. drums only)

    return result;
}

} // namespace kerf
