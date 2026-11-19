#include "flanger.h"

#include <complex>

namespace fmvibe {

FlangerSweep flangerSweep(double depth) {
    const double reach = 0.95 * std::clamp(depth, 0.0, 1.0);
    return {std::sqrt((1.0 - reach) * (1.0 + reach)), 0.5 * std::log((1.0 + reach) / (1.0 - reach))};
}

double flangerDelay(double delayNorm, double depth, double lfo) {
    const FlangerSweep sweep = flangerSweep(depth);
    return timeFromNorm(delayNorm) * sweep.middle * std::exp(sweep.reach * lfo);
}

double flangerResponse(double hz, double delaySeconds, double feedback, double mix, double polarity) {
    using Complex = std::complex<double>;
    const Complex delay = std::polar(1.0, -2.0 * std::numbers::pi * hz * delaySeconds);
    const double g = polarity * sweepFeedback(feedback);
    return std::abs(1.0 - mix + polarity * mix * sweepGain(feedback) * delay / (1.0 - g * delay));
}

namespace fx {

namespace {

constexpr double kMaxFlangerDelay = 0.032; // the longest center delay at full depth, with room

} // namespace

void Flanger::prepare(double sampleRate) {
    for (ZeroBuffer &line : lines)
        line.assign(static_cast<size_t>(std::ceil(kMaxFlangerDelay * sampleRate)) + 4, 0.0f);
    for (ZeroBuffer &line : dryLines)
        line.assign(static_cast<size_t>(std::ceil(kMaxFlangerDelay * sampleRate)) + 4, 0.0f);
}

void Flanger::start(const Context &ctx) {
    for (ZeroBuffer &line : lines)
        line.zero(ran);
    for (ZeroBuffer &line : dryLines)
        line.zero(ran);
    ran = 0;
    write = 0;
    last = {};
    dryDelay = 0.0; // taken as it is, below
    throughZero = ctx.at(FxParam::FlangerThroughZero) >= 0.5 ? 1.0 : 0.0;
    phase = 0.0;
    delayKnown = false;
    mix = sweepWet(ctx.at(FxParam::FlangerMix));
    polarity = sweepPolarity(ctx.at(FxParam::FlangerNegative));
}

void Flanger::idle(double cycles) {
    phase += cycles;
    phase -= std::floor(phase);
    delayKnown = false; // worked out again where the sweep is now
}

double Flanger::run(const Context &ctx, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const ParamValues &p = ctx.p;
    const double sampleRate = ctx.sampleRate;
    const double step = sweepHz([&p](int i) { return p[i]; }, ctx.slot, FxType::Flanger, ctx.tempo) / sampleRate;
    const FlangerSweep sweep = flangerSweep(ctx.at(FxParam::FlangerDepth)); // flangerDelay()'s, the same all chunk
    const double middleSamples = timeFromNorm(ctx.at(FxParam::FlangerDelay)) * sweep.middle * sampleRate;
    const double feedback = sweepFeedback(ctx.at(FxParam::FlangerFeedback));
    const double gain = sweepGain(ctx.at(FxParam::FlangerFeedback));
    const double mixStep = rampStep(mix, sweepWet(ctx.at(FxParam::FlangerMix)), n, ctx.rampShortest);
    const double polarityTo = sweepPolarity(ctx.at(FxParam::FlangerNegative));
    const double polarityStep = 2.0 / (kPolaritySeconds * sampleRate);
    const double spread = ctx.at(FxParam::FlangerStereo) >= 0.5 ? 0.25 : 0.0; // the right side's head start
    const int size = static_cast<int>(lines[0].size());
    // Through zero: the dry sound as late as the sweep's middle, gliding there like a tape's (and, starting, there
    // at once), and how far it has gone over to it.
    const double dryTarget = std::clamp(middleSamples, 3.0, size - 3.0);
    if (dryDelay <= 0.0)
        dryDelay = dryTarget;
    const double dryGlide = 1.0 - std::exp(-1.0 / (0.02 * sampleRate));
    const double zeroTo = ctx.at(FxParam::FlangerThroughZero) >= 0.5 ? 1.0 : 0.0;
    const double zeroStep = 1.0 / (kThroughZeroFadeSeconds * sampleRate);
    // The delay in samples where the sweep is at `phase`, on each side: worked out every kSweepStep samples, as the
    // phaser's coefficient is (see Phaser::run()). 3 samples at the least, for its cubic.
    auto delayAt = [&](double at, int ch) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (at + spread * ch));
        return std::clamp(middleSamples * std::exp(sweep.reach * lfo), 3.0, size - 3.0); // flangerDelay()
    };
    if (!delayKnown) {
        delay = {delayAt(phase, 0), delayAt(phase, 1)};
        delayKnown = true;
    }
    std::array<double, 2> delayTo{}, delayStep{};
    uint32_t stepLeft = 0;
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            double ahead = phase;
            for (uint32_t k = 0; k < stepLeft; ++k) {
                ahead += step;
                ahead -= std::floor(ahead);
            }
            for (int ch = 0; ch < 2; ++ch) {
                delayTo[ch] = delayAt(ahead, ch);
                delayStep[ch] = (delayTo[ch] - delay[ch]) / stepLeft;
            }
        }
        --stepLeft;
        mix += mixStep;
        polarity = moveToward(polarity, polarityTo, polarityStep);
        phase += step;
        phase -= std::floor(phase);
        throughZero = moveToward(throughZero, zeroTo, zeroStep);
        const bool zeroing = throughZero > 0.0;
        if (zeroing)
            dryDelay += (dryTarget - dryDelay) * dryGlide;
        // The dry sound's share as it goes over to the delayed one, along a raised cosine.
        const double late = zeroing ? 0.5 - 0.5 * std::cos(std::numbers::pi * throughZero) : 0.0;
        for (int ch = 0; ch < 2; ++ch) {
            delay[ch] = stepLeft == 0 ? delayTo[ch] : delay[ch] + delayStep[ch];
            // On a cubic: through a straight line between two samples, what goes round would lose its top and come
            // back grainy as the sweep moves.
            const double delayed = readCubic(lines[ch], write, delay[ch]);
            const double in = io[ch][s];
            dryLines[ch][write] = static_cast<float>(in); // always, so through zero starts with what came before
            const double x = zeroing ? in + (readCubic(dryLines[ch], write, dryDelay) - in) * late : in;
            lines[ch][write] = static_cast<float>(in + polarity * feedback * delayed);
            last[ch] = delayed;
            io[ch][s] = static_cast<float>(x + (polarity * delayed * gain - x) * mix);
            peak = std::max({peak, std::abs(in), std::abs(delayed)});
        }
        write = write + 1 == size ? 0 : write + 1;
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
