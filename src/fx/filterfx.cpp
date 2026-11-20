#include "filterfx.h"

#include <cstring>
#include <initializer_list>

namespace fmvibe::fx {

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
    ramp.movedLast = false;
    fmLast = 0.0;
}

void FilterFx::settle(const FilterInputs &now) {
    if (std::memcmp(now.data(), settingsFor.data(), sizeof now) == 0)
        return;
    settingsFor = now;
    settings = filterSettingsFrom(now, rate);
}

bool FilterFx::rampStepAt(uint32_t s) {
    if (s % kSweepStep == 0) { // the next step: its settings as they will stand at its end
        const uint32_t to = std::min(s + kSweepStep, ramp.n);
        FilterInputs now = inputsNow;
        for (size_t i = 0; i < now.size(); ++i)
            if (kFilterInputSmooth[i])
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
    // Worked out again only when its params, or the pitch it tracks, move (see filterInputs()).
    const FilterInputs inputs =
        filterInputs(ctx.at(FxParam::FilterCutoff), ctx.at(FxParam::FilterKeyTrack), ctx.pitch,
                     ctx.at(FxParam::FilterType), ctx.at(FxParam::FilterSlope), ctx.at(FxParam::FilterResonance),
                     ctx.at(FxParam::FilterDrive), ctx.at(FxParam::FilterVowel), ctx.at(FxParam::FilterMorph),
                     ctx.at(FxParam::FilterMorphMode));
    // The inputs that move the filter smoothly ramp to the chunk's (as a gain does, see rampStep()), so modulation
    // doesn't step it every control block; the type, the slope and the morph's mode switch at once (and a new type
    // takes every input at once, as it starts anew). While they ramp, its settings are worked out again every
    // kSweepStep samples, at the step's end, and what it scales its outputs by glides there from the step before,
    // sample by sample (see FilterGains).
    if (std::isnan(inputsNow[0]) || inputsNow[FilterIn::Type] != inputs[FilterIn::Type])
        inputsNow = inputs;
    ramp.arrives = true;
    for (size_t i = 0; i < inputs.size(); ++i) {
        ramp.step[i] = 0.0;
        if (!kFilterInputSmooth[i]) {
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
    if (!ramp.moving) {
        settle(inputs);
    } else if (!ramp.movedLast) { // as it was left, the first step's start: held still at its settings
        settle(ramp.from);
        ramp.gainsTo = filterGains(settings);
    } // else where the last chunk's ramp left them, its last step's end
    ramp.movedLast = ramp.moving;
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
