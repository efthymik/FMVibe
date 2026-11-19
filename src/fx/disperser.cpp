#include "disperser.h"

#include <complex>

namespace fmvibe {

double disperserCenter(double freqNorm, double keyTrack, double pitch) {
    // Without key tracking, Freq exactly: 2 to the power of 0 is 1.
    return cutoffFromNorm(freqNorm) * std::exp2((pitch - 60.0) / 12.0 * keyTrack);
}

int disperserStretch(double stretch, double sampleRate) {
    const double samples = std::clamp(std::round(stretch), 1.0, static_cast<double>(kMaxDisperserStretch));
    return std::max(1, static_cast<int>(std::lround(samples * sampleRate / 48000.0)));
}

std::array<double, 5> disperserCoefs(double hz, double pinch, double sampleRate, int stretch) {
    // Stretched, where `stretch` times `hz` lands once folded back into the band: there the plain all-pass puts the
    // dispersion that, stretched, lands on `hz`.
    // Close to 0 or the half rate, where two copies meet, it is held back a little: an all-pass there would ring on
    // for good.
    double designed = std::clamp(hz, 10.0, 0.45 * sampleRate);
    if (stretch > 1) {
        designed = std::fmod(designed * stretch, sampleRate);
        if (designed > 0.5 * sampleRate)
            designed = sampleRate - designed;
        designed = std::clamp(designed, 0.02 * sampleRate, 0.48 * sampleRate);
    }
    const double w = 2.0 * std::numbers::pi * designed / sampleRate;
    const double q = 0.3 * std::pow(20.0, std::clamp(pinch, 0.0, 1.0));
    const double alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    return {(1.0 - alpha) / a0, -2.0 * std::cos(w) / a0, 1.0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

double disperserDelay(double at, double hz, double pinch, int stages, double sampleRate, int stretch) {
    // An all-pass's delay is how fast its phase turns with frequency; stretched, its phase at `w` is the plain one's
    // at `stretch` times `w`, turning `stretch` times as fast.
    const std::array<double, 5> c = disperserCoefs(hz, pinch, sampleRate, stretch);
    auto phase = [&](double w) {
        const std::complex<double> z1 = std::polar(1.0, -w * stretch), z2 = z1 * z1;
        return std::arg((c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2));
    };
    const double w = 2.0 * std::numbers::pi * std::min(at, 0.45 * sampleRate) / sampleRate;
    const double dw = (1e-4 * w + 1e-7) / stretch;
    double turn = phase(w + dw) - phase(w - dw);
    turn -= 2.0 * std::numbers::pi * std::round(turn / (2.0 * std::numbers::pi));
    return -turn / (2.0 * dw) * stages / sampleRate;
}

namespace fx {

void Disperser::prepare(double sampleRate) {
    ringLength = disperserStretch(kMaxDisperserStretch, sampleRate);
    rings.assign(static_cast<size_t>(kRingStride) * static_cast<size_t>(ringLength));
    stretch = 0;
}

void Disperser::clear() {
    stages = {};
    // The places along the rings it used: the first `stretch` (nothing to do while they were never written).
    rings.zero(static_cast<size_t>(kRingStride) * static_cast<size_t>(std::max(stretch, 0)));
    ringAt = 0;
    ringStages = 0;
}

void Disperser::start(const Context &ctx) {
    clear();
    coefs = BiquadRamp{};
    mix = ctx.at(FxParam::DisperserMix);
    playing = fadingFrom = 0;
    fade = 0.0;
    stretch = 0;
    heard = 1.0;
}

double Disperser::run(const Context &ctx, float *const *io, uint32_t n) {
    // Stretch, in samples: a new one waits until the sound has faded out to what goes in, then starts from silence.
    const int wanted =
        std::min(disperserStretch(ctx.at(FxParam::DisperserStretch), ctx.sampleRate), std::max(ringLength, 1));
    if (stretch == 0)
        stretch = wanted;
    const double heardStep = 1.0 / (kDisperserStretchFadeSeconds * ctx.sampleRate);
    // Back in over as long as the stretch it plays (see kDisperserFadeInSeconds).
    auto inStep = [&] {
        const double seconds = std::clamp(kDisperserFadeInSeconds * stretch * 48000.0 / ctx.sampleRate,
                                          kDisperserMinFadeInSeconds, kDisperserMaxFadeInSeconds);
        return 1.0 / (seconds * ctx.sampleRate);
    };
    double fadeIn = inStep();
    const double hz = disperserCenter(ctx.at(FxParam::DisperserFreq), ctx.at(FxParam::DisperserKeyTrack), ctx.pitch);
    const std::array<double, 5> b = disperserCoefs(hz, ctx.at(FxParam::DisperserPinch), ctx.sampleRate, stretch);
    coefs.begin({b[0], b[1], b[2], b[3], b[4]}, n, ctx.rampShortest);
    const int count =
        std::clamp(static_cast<int>(std::lround(ctx.at(FxParam::DisperserStages))), 1, kMaxDisperserStages);
    if (playing == 0)
        playing = count;
    if (count != playing && fade <= 0.0) {
        fadingFrom = playing;
        playing = count;
        fade = 1.0;
    }
    // It runs the longer chain, and takes the shorter one's output on the way.
    const int running = fade > 0.0 ? std::max(playing, fadingFrom) : playing;
    const int shorter = fade > 0.0 ? std::min(playing, fadingFrom) : playing;
    for (int ch = 0; ch < 2; ++ch)
        for (int k = running; k < kMaxDisperserStages; ++k)
            stages[ch][k] = Biquad{};
    if (stretch > 1 && ringStages > running) { // so are the stretched ones, along every place they use
        for (int at = 0; at < stretch; ++at)
            for (int ch = 0; ch < 2; ++ch) {
                double *place = rings.data() + static_cast<ptrdiff_t>(at) * kRingStride + 2 * kMaxDisperserStages * ch;
                std::fill(place + 2 * running, place + 2 * ringStages, 0.0);
            }
    }
    ringStages = running;
    const double fadeStep = 1.0 / (kDisperserFadeSeconds * ctx.sampleRate);
    const double mixStep = rampStep(mix, ctx.at(FxParam::DisperserMix), n, ctx.rampShortest);
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        mix += mixStep;
        if (wanted != stretch || heard < 1.0) {
            heard = wanted != stretch ? std::max(heard - heardStep, 0.0) : std::min(heard + fadeIn, 1.0);
            if (heard <= 0.0 && wanted != stretch) { // faded out: the new length, from silence
                clear();
                stretch = wanted;
                ringStages = running; // which the rest of the chunk writes
                fadeIn = inStep();
                const std::array<double, 5> d =
                    disperserCoefs(hz, ctx.at(FxParam::DisperserPinch), ctx.sampleRate, stretch);
                coefs.jump({d[0], d[1], d[2], d[3], d[4]});
            }
        }
        coefs.next();
        const BiquadCoefs &c = coefs.now;
        // The count it fades out from, along a raised cosine.
        double was = 0.0;
        if (fade > 0.0) {
            fade = std::max(fade - fadeStep, 0.0);
            was = 0.5 - 0.5 * std::cos(std::numbers::pi * fade);
        }
        // How much of its sound comes out as Stretch changes, along a raised cosine too.
        const double shown = heard >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(std::numbers::pi * heard);
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            double y = x, first = x;
            if (stretch == 1) {
                std::array<Biquad, kMaxDisperserStages> &chain = stages[ch];
                for (int k = 0; k < shorter; ++k)
                    y = chain[k].process(y, c);
                first = y;
                for (int k = shorter; k < running; ++k)
                    y = chain[k].process(y, c);
            } else {
                // Transposed direct form II with each delay `stretch` samples long: what it reads at ringAt was
                // written `stretch` samples ago, and what it writes there is read `stretch` samples on.
                double *place = rings.data() + static_cast<ptrdiff_t>(ringAt) * kRingStride +
                                2 * kMaxDisperserStages * ch;
                auto stage = [&](int k, double in) {
                    double &z1 = place[2 * k], &z2 = place[2 * k + 1];
                    const double out = c.b0 * in + z1;
                    z1 = c.b1 * in - c.a1 * out + z2;
                    z2 = c.b2 * in - c.a2 * out;
                    return out;
                };
                for (int k = 0; k < shorter; ++k)
                    y = stage(k, y);
                first = y;
                for (int k = shorter; k < running; ++k)
                    y = stage(k, y);
            }
            const double now = playing == shorter ? first : y;
            const double before = playing == shorter ? y : first;
            double wet = was > 0.0 ? now + (before - now) * was : now;
            if (shown < 1.0)
                wet = x + (wet - x) * shown;
            io[ch][s] = static_cast<float>(x + (wet - x) * mix);
            peak = std::max({peak, std::abs(x), std::abs(wet)});
        }
        if (stretch > 1)
            ringAt = ringAt + 1 >= stretch ? 0 : ringAt + 1;
    }
    coefs.end();
    return peak;
}

} // namespace fx
} // namespace fmvibe
