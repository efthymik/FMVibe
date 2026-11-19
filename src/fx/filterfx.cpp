#include "filterfx.h"

#include <cstring>
#include <initializer_list>

namespace fmvibe::fx {

namespace {

// The inputs that move the filter smoothly (its cutoff, key tracking, the pitch it tracks, resonance, drive, vowel and
// morph) and ramp; its type, slope and morph mode switch at once.
constexpr std::array<bool, 10> kSmooth{false, true, true, true, false, true, true, true, true, false};

} // namespace

void FilterFx::prepare(double sampleRate) {
    for (ZeroBuffer &comb : combs) // the filter runs at twice the rate (see run())
        comb.assign(static_cast<size_t>(combLength(2.0 * sampleRate)), 0.0f);
    settingsFor.fill(std::numeric_limits<double>::quiet_NaN()); // for this rate
    inputsNow.fill(std::numeric_limits<double>::quiet_NaN());
    rate = 2.0 * sampleRate;
    fmRange = cutoffFmRange(2.0 * sampleRate);
    const double transition = halfBandTransition(sampleRate);
    for (auto *bands : {&up, &down})
        for (HalfBand &band : *bands)
            band.design(HalfBand::kMaxCoefs, transition);
}

void FilterFx::start(const Context &ctx) {
    channels = {};
    for (ZeroBuffer &comb : combs)
        comb.zero();
    for (int ch = 0; ch < 2; ++ch) {
        up[ch].reset();
        down[ch].reset();
    }
    mix = ctx.at(FxParam::FilterMix);
    inputsNow.fill(std::numeric_limits<double>::quiet_NaN()); // taken as they are, from silence
    fmLast = 0.0;
}

FilterSettings FilterFx::make(const std::array<double, 10> &now) const {
    // Key tracking moves the cutoff with the pitch from C4 on, as a voice filter's does. Without it the pitch is 0 here
    // (left out), and 2 to the power of 0 is exactly 1.
    const auto type = static_cast<FilterType>(
        std::clamp(static_cast<int>(std::lround(now[0])), 0, static_cast<int>(FilterType::Count) - 1));
    const double cutoffHz = cutoffFromNorm(now[1]) * std::exp2((now[3] - 60.0) / 12.0 * now[2]);
    return filterSettings(type, filterSlope(now[4]), cutoffHz, now[5], now[6], rate, now[7], now[8], morphMode(now[9]));
}

void FilterFx::settle(const std::array<double, 10> &now) {
    if (std::memcmp(now.data(), settingsFor.data(), sizeof now) == 0)
        return;
    settingsFor = now;
    settings = make(now);
}

bool FilterFx::rampStepAt(uint32_t s) {
    if (s % kSweepStep == 0) { // the next step: its settings as they will stand at its end
        const uint32_t to = std::min(s + kSweepStep, ramp.n);
        std::array<double, 10> now = inputsNow;
        for (size_t i = 0; i < now.size(); ++i)
            if (kSmooth[i])
                now[i] = to == ramp.n && ramp.arrives ? ramp.to[i] : ramp.from[i] + ramp.step[i] * to;
        ramp.gainsFrom = ramp.gainsTo;
        settingsFor.fill(std::numeric_limits<double>::quiet_NaN()); // its gains are gliding: worked out anew
        settle(now);
        ramp.gainsTo = filterGains(settings);
        ramp.length = to - s;
        glideFilterGains(settings, ramp.gainsFrom, ramp.gainsTo, 1.0 / ramp.length);
        return true;
    }
    glideFilterGains(settings, ramp.gainsFrom, ramp.gainsTo, static_cast<double>(s % kSweepStep + 1) / ramp.length);
    return false;
}

double FilterFx::runModulated(const float *octaves, float *const *io, uint32_t n, double mixStep) {
    FilterSettings moved = settings;
    const int combSize = static_cast<int>(combs[0].size());
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        // While its inputs ramp, the settings it moves from move too: taken up whole at each step, what it scales its
        // outputs by gliding in between, on top of the cutoff the modulator takes.
        bool stepped = false;
        if (ramp.moving)
            stepped = rampStepAt(s);
        if (stepped)
            moved = settings;
        mix += mixStep;
        double twice[2][2];
        for (int ch = 0; ch < 2; ++ch)
            up[ch].processUp(io[ch][s], twice[ch][0], twice[ch][1]);
        // The modulator comes at the host's rate: half way to this sample's, then there.
        const double to = octaves[s];
        for (int t = 0; t < 2; ++t) {
            // The diode's stages take a system solved anew: every other sample of these is enough at twice the rate.
            if (t == 0 || moved.type != FilterType::Diode)
                modulateCutoff(moved, settings, std::exp2(t == 0 ? 0.5 * (fmLast + to) : to), fmRange);
            if (ramp.moving)
                glideFilterGains(moved, ramp.gainsFrom, ramp.gainsTo,
                                 static_cast<double>(s % kSweepStep + 1) / ramp.length);
            for (int ch = 0; ch < 2; ++ch) {
                double &x = twice[ch][t];
                const double y = channels[ch].process(x, moved, combs[ch].data(), combSize);
                peak = std::max(peak, std::abs(y));
                x += (y - x) * mix;
            }
        }
        fmLast = to;
        for (int ch = 0; ch < 2; ++ch)
            io[ch][s] = static_cast<float>(down[ch].process(twice[ch][0], twice[ch][1]));
    }
    return peak;
}

double FilterFx::run(const Context &ctx, float *const *io, uint32_t n) {
    const double pitch = ctx.pitch;
    // Worked out again only when its params, or the pitch it tracks, move. Without key tracking the pitch moves
    // nothing (2 to the power of 0 is exactly 1), and is left out.
    const double keyTrack = ctx.at(FxParam::FilterKeyTrack);
    // The Formant type's vowel and the Morph type's place and mode go in for those types alone, as a voice filter's.
    const int typeNumber = static_cast<int>(std::lround(ctx.at(FxParam::FilterType)));
    const auto playing = static_cast<FilterType>(std::clamp(typeNumber, 0, static_cast<int>(FilterType::Count) - 1));
    const bool formant = playing == FilterType::Formant, morph = playing == FilterType::Morph;
    const std::array<double, 10> inputs{ctx.at(FxParam::FilterType),
                                       ctx.at(FxParam::FilterCutoff),
                                       keyTrack,
                                       keyTrack != 0.0 ? pitch : 0.0,
                                       ctx.at(FxParam::FilterSlope),
                                       ctx.at(FxParam::FilterResonance),
                                       ctx.at(FxParam::FilterDrive),
                                       formant ? ctx.at(FxParam::FilterVowel) : 0.0,
                                       morph ? ctx.at(FxParam::FilterMorph) : 0.0,
                                       morph ? ctx.at(FxParam::FilterMorphMode) : 0.0};
    // The inputs that move the filter smoothly ramp to the chunk's (as a gain does, see rampStep()), so modulation
    // doesn't step it every control block; the type, the slope and the morph's mode switch at once (and a new type
    // takes every input at once, as it starts anew). While they ramp, its settings are worked out again every
    // kSweepStep samples, at the step's end, and what it scales its outputs by glides there from the step before,
    // sample by sample (see FilterGains).
    if (std::isnan(inputsNow[0]) || inputsNow[0] != inputs[0])
        inputsNow = inputs;
    ramp.arrives = true;
    for (size_t i = 0; i < inputs.size(); ++i) {
        ramp.step[i] = 0.0;
        if (!kSmooth[i]) {
            inputsNow[i] = inputs[i];
            continue;
        }
        ramp.step[i] = rampStep(inputsNow[i], inputs[i], n, ctx.rampShortest);
        ramp.arrives = ramp.arrives && rampEnds(inputsNow[i], inputs[i], n, ctx.rampShortest);
    }
    ramp.moving = inputsNow != inputs;
    ramp.from = inputsNow;
    ramp.to = inputs;
    ramp.n = n;
    if (!ramp.moving)
        settle(inputs);
    else
        ramp.gainsTo = filterGains(make(ramp.from)); // as it was left, the first step's start
    // At twice the rate, as the voices' filters run at their oversampling: the analog types saturate inside their
    // loops and Drive before them, whose harmonics fold back far less there, and Drive's anti-aliasing, an average
    // over each step, would dull the top at the host rate. What goes past (Mix) goes up and down the same way, so the
    // two stay in step.
    const double mixStep = rampStep(mix, ctx.at(FxParam::FilterMix), n, ctx.rampShortest);
    double peak = 0.0;
    if (ctx.cutoffOctaves) {
        peak = runModulated(ctx.cutoffOctaves, io, n, mixStep);
    } else {
        fmLast = 0.0;
        const int combSize = static_cast<int>(combs[0].size());
        for (uint32_t s = 0; s < n; ++s) {
            if (ramp.moving)
                rampStepAt(s);
            mix += mixStep;
            for (int ch = 0; ch < 2; ++ch) {
                double twice[2];
                up[ch].processUp(io[ch][s], twice[0], twice[1]);
                for (double &x : twice) {
                    const double y = channels[ch].process(x, settings, combs[ch].data(), combSize);
                    peak = std::max(peak, std::abs(y));
                    x += (y - x) * mix;
                }
                io[ch][s] = static_cast<float>(down[ch].process(twice[0], twice[1]));
            }
        }
    }
    if (ramp.moving) {
        for (size_t i = 0; i < inputs.size(); ++i)
            inputsNow[i] = ramp.arrives ? inputs[i] : ramp.from[i] + ramp.step[i] * n;
        settingsFor.fill(std::numeric_limits<double>::quiet_NaN()); // the gains it left glided; worked out anew next
    }
    return peak;
}

} // namespace fmvibe::fx
