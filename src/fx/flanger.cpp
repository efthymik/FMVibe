#include "flanger.h"

#include "../fxlag.h"

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

// The longest a line's read reaches: the sweep's longest end (1.95 times the longest Delay, 15 ms), or through zero
// the dry sound's wait on top of the most the sweep runs past its middle (1.64 times it), with room.
constexpr double kMaxFlangerDelay = 0.034;

} // namespace

void Flanger::prepare(double sampleRate) {
    for (ZeroBuffer &line : lines)
        line.assign(static_cast<size_t>(std::ceil(kMaxFlangerDelay * sampleRate)) + 4, 0.0f);
    for (ZeroBuffer &line : dryLines)
        line.assign(static_cast<size_t>(throughZeroWaitSamples(sampleRate)), 0.0f);
}

void Flanger::start(const Context &ctx) {
    for (ZeroBuffer &line : lines)
        line.zero(ran);
    for (ZeroBuffer &line : dryLines)
        line.zero(ran);
    ran = 0;
    write = dryWrite = 0;
    last = {};
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
    const int wait = static_cast<int>(dryLines[0].size()); // read before it is written over, `wait` samples on
    const double zeroTo = ctx.at(FxParam::FlangerThroughZero) >= 0.5 ? 1.0 : 0.0;
    const double zeroStep = 1.0 / (kThroughZeroFadeSeconds * sampleRate);
    // The delay in samples where the sweep is at `phase`, on each side, and through zero where the swept copy is read
    // (the dry sound's wait, then as far off the sweep's middle as the sweep is): worked out every kSweepStep samples,
    // as the phaser's coefficient is (see Phaser::run()). 3 samples at the least, for its cubic.
    auto sweepAt = [&](double at, int ch, double &zeroAt) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (at + spread * ch));
        const double swept = middleSamples * std::exp(sweep.reach * lfo); // flangerDelay()
        zeroAt = std::clamp(wait + swept - middleSamples, 3.0, size - 3.0);
        return std::clamp(swept, 3.0, size - 3.0);
    };
    if (!delayKnown) {
        for (int ch = 0; ch < 2; ++ch)
            delay[ch] = sweepAt(phase, ch, zeroDelay[ch]);
        delayKnown = true;
    }
    std::array<double, 2> delayTo{}, delayStep{}, zeroDelayTo{}, zeroDelayStep{};
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
                delayTo[ch] = sweepAt(ahead, ch, zeroDelayTo[ch]);
                delayStep[ch] = (delayTo[ch] - delay[ch]) / stepLeft;
                zeroDelayStep[ch] = (zeroDelayTo[ch] - zeroDelay[ch]) / stepLeft;
            }
        }
        --stepLeft;
        mix += mixStep;
        polarity = moveToward(polarity, polarityTo, polarityStep);
        phase += step;
        phase -= std::floor(phase);
        throughZero = moveToward(throughZero, zeroTo, zeroStep);
        // The share of the sound through zero as it goes over to it, along a raised cosine.
        const double late = throughZero >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(std::numbers::pi * throughZero);
        for (int ch = 0; ch < 2; ++ch) {
            delay[ch] = stepLeft == 0 ? delayTo[ch] : delay[ch] + delayStep[ch];
            zeroDelay[ch] = stepLeft == 0 ? zeroDelayTo[ch] : zeroDelay[ch] + zeroDelayStep[ch];
            // On a cubic: through a straight line between two samples, what goes round would lose its top and come
            // back grainy as the sweep moves.
            const double delayed = readCubic(lines[ch], write, delay[ch]);
            const double in = io[ch][s];
            // The dry sound always goes through its line, so through zero starts with what came before.
            const double waited = dryLines[ch][dryWrite];
            dryLines[ch][dryWrite] = static_cast<float>(in);
            double x = in, wet = delayed;
            if (late > 0.0) {
                const double zeroed = readCubic(lines[ch], write, zeroDelay[ch]);
                x = in + (waited - in) * late;
                wet = delayed + (zeroed - delayed) * late;
            }
            lines[ch][write] = static_cast<float>(in + polarity * feedback * delayed);
            last[ch] = delayed;
            io[ch][s] = static_cast<float>(x + (polarity * wet * gain - x) * mix);
            peak = std::max({peak, std::abs(in), std::abs(delayed), std::abs(waited)});
        }
        write = write + 1 == size ? 0 : write + 1;
        dryWrite = dryWrite + 1 == wait ? 0 : dryWrite + 1;
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
