#include "filterfx.h"

#include <cstring>
#include <initializer_list>

namespace fmvibe::fx {

void FilterFx::prepare(double sampleRate) {
    for (ZeroBuffer &comb : combs) // the filter runs at twice the rate (see run())
        comb.assign(static_cast<size_t>(combLength(2.0 * sampleRate)), 0.0f);
    settingsFor.fill(std::numeric_limits<double>::quiet_NaN()); // for this rate
    inputsNow.fill(std::numeric_limits<double>::quiet_NaN());
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
    // The inputs that move the filter smoothly ramp to the chunk's; the type, the slope and the morph's mode switch
    // at once (and a new type takes every input at once, as it starts anew).
    constexpr std::array<bool, 10> kSmooth{false, true, true, true, false, true, true, true, true, false};
    if (std::isnan(inputsNow[0]) || inputsNow[0] != inputs[0])
        inputsNow = inputs;
    std::array<double, 10> step{};
    bool arrives = true;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!kSmooth[i]) {
            inputsNow[i] = inputs[i];
            continue;
        }
        step[i] = rampStep(inputsNow[i], inputs[i], n, ctx.rampShortest);
        arrives = arrives && rampEnds(inputsNow[i], inputs[i], n, ctx.rampShortest);
    }
    const bool moving = inputsNow != inputs;
    const std::array<double, 10> from = inputsNow;
    // Worked out again only when its inputs move: every kSweepStep samples while they ramp, at the step's end.
    auto settle = [&](const std::array<double, 10> &now) {
        if (std::memcmp(now.data(), settingsFor.data(), sizeof now) == 0)
            return;
        settingsFor = now;
        const auto type = static_cast<FilterType>(
            std::clamp(static_cast<int>(std::lround(now[0])), 0, static_cast<int>(FilterType::Count) - 1));
        // Key tracking moves the cutoff with the pitch from C4 on, as a voice filter's does.
        const double cutoffHz = cutoffFromNorm(now[1]) * std::exp2((now[3] - 60.0) / 12.0 * now[2]);
        settings = filterSettings(type, filterSlope(now[4]), cutoffHz, now[5], now[6], 2.0 * ctx.sampleRate, now[7],
                                  now[8], morphMode(now[9]));
    };
    if (!moving)
        settle(inputs);
    // At twice the rate, as the voices' filters run at their oversampling: the analog types saturate inside their
    // loops and Drive before them, whose harmonics fold back far less there, and Drive's anti-aliasing, an average
    // over each step, would dull the top at the host rate. What goes past (Mix) goes up and down the same way, so the
    // two stay in step.
    const double mixStep = rampStep(mix, ctx.at(FxParam::FilterMix), n, ctx.rampShortest);
    const int combSize = static_cast<int>(combs[0].size());
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        if (moving && s % kSweepStep == 0) {
            const uint32_t to = std::min(s + kSweepStep, n);
            std::array<double, 10> now = inputsNow;
            for (size_t i = 0; i < now.size(); ++i)
                if (kSmooth[i])
                    now[i] = to == n && arrives ? inputs[i] : from[i] + step[i] * to;
            settle(now);
        }
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
    if (moving)
        for (size_t i = 0; i < inputs.size(); ++i)
            inputsNow[i] = arrives ? inputs[i] : from[i] + step[i] * n;
    return peak;
}

} // namespace fmvibe::fx
