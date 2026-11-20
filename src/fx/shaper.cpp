#include "shaper.h"

#include <initializer_list>
#include <limits>

namespace fmvibe::fx {

namespace {

// How long a shaper effect's path, going to Stack or leaving it, takes to fade over to the other (see run()), as
// the amp's settings do: a stack's stages start from silence, and its level as it is driven is unlike the others'.
constexpr double kShaperPathFade = 0.01; // seconds

} // namespace

void Shaper::prepare(double sampleRate) {
    const double transition = halfBandTransition(sampleRate);
    for (auto *bands : {&up, &down})
        for (HalfBand &band : *bands)
            band.design(HalfBand::kMaxCoefs, transition);
    for (int k = 0; k < 2; ++k)
        for (int ch = 0; ch < 2; ++ch) {
            ups[k][ch].prepare(sampleRate, 4 << k);
            downs[k][ch].prepare(sampleRate, 4 << k);
        }
    factor = 0;
}

namespace {

// Tone's shelves at `rate`: the one before the curve, flat at 0 Hz and `tone` times kShaperToneDb up at the top, and
// the one after it, exactly its opposite (both from the same bilinear transform, so the one undoes the other).
std::array<BiquadCoefs, 2> toneShelves(double tone, double rate) {
    if (tone == 0.0)
        return {BiquadCoefs{}, BiquadCoefs{}};
    // The analog (A s + w) / (s + w), w prewarped, through the bilinear transform.
    const double a = std::pow(10.0, std::clamp(tone, -1.0, 1.0) * kShaperToneDb / 20.0);
    const double k = std::tan(std::numbers::pi * std::min(kShaperToneHz, 0.45 * rate) / rate); // w / (2 rate)
    const double b0 = (a + k) / (1.0 + k), b1 = (k - a) / (1.0 + k), a1 = (k - 1.0) / (1.0 + k);
    return {BiquadCoefs{b0, b1, 0.0, a1, 0.0}, BiquadCoefs{1.0 / b0, a1 / b0, 0.0, b1 / b0, 0.0}};
}

} // namespace

void Shaper::start(const Context &ctx) {
    const ParamValues &p = ctx.p;
    const ShaperSettings settings = shaperSettings([&p](int index) { return p[index]; }, shaperParams(ctx.slot));
    sides = {};
    // Its drawn curve too, taken at once as it starts rather than gliding in from the one it last ran with (a
    // patch or two ago, say).
    curve.built = false;
    curve.points.fill(std::numeric_limits<double>::quiet_NaN());
    drive = settings.drive;
    output = settings.output;
    mix = settings.mix;
    between = settings.between;
    ran = false; // its path starts as it is, from silence, fading in from nothing it left
    wasFade = 0.0;
    factor = 0;
    heard = 1.0;
    tones = {};
    toneCoefs = {};
}

// The voices' shapers run the same way (see Synth::renderBlock()), on what goes into them, at their oversampling; the
// shaper effect runs at twice the host rate. The anti-aliasing averages the curve over each sample step, which for what
// the curve leaves straight is the mean of two samples: at the host rate, a top falling away toward its half, 5 dB at
// 15 kHz at 48 kHz for a quiet sound the curve leaves alone, and a stack's further, stage by stage (see kStackLift);
// at twice it, about a dB. Going to Stack or leaving it under sound, the path it leaves runs on for kShaperPathFade on
// the same samples, fading out as the new one fades in.
void Shaper::run(const Context &ctx, float *const *io, uint32_t n) {
    const ParamValues &p = ctx.p;
    auto get = [&p](int index) { return p[index]; };
    const ShaperParams fields = shaperParams(ctx.slot);
    const ShaperSettings settings = shaperSettings(get, fields);
    if (settings.mode == ClipMode::Custom) { // a drawn curve glides to new points
        curve.update(get, fields.curve);
        curve.advance(n / ctx.sampleRate);
    }
    const bool isStack = settings.mode == ClipMode::Stack;
    if (ran && isStack != stacked) {
        // The path it leaves fades out from where it is (one already fading out, which this one replaces, goes).
        was = last;
        wasSides = sides;
        wasFade = 1.0;
    }
    // Its oversampling: a new one waits until it has ducked out, then starts from silence (its paths and filters
    // cleared), and it ducks back in.
    const int wanted = shaperOversampling(ctx.at(FxParam::ShaperOversampling));
    if (factor == 0)
        factor = wanted;
    if (!ran) // from silence, rather than from what they last held
        for (int ch = 0; ch < 2; ++ch) {
            up[ch].reset();
            down[ch].reset();
            for (int k = 0; k < 2; ++k) {
                ups[k][ch].reset();
                downs[k][ch].reset();
            }
        }
    stacked = isStack;
    ran = true;
    last = {settings.mode, settings.symmetric, settings.stack};
    const double rate = factor * ctx.sampleRate;
    ClipCurve now = curve.curve(settings.mode, settings.symmetric);
    now.stack(settings.stack, rate);
    const double fadeStep = 1.0 / n;
    const bool block = clipNeedsBlocker(settings.mode);
    const double pole = clipBlockerPole(rate);
    // The path it left.
    ClipCurve wasCurve = curve.curve(was.mode, was.symmetric);
    wasCurve.stack(was.stack, rate);
    const bool wasBlock = clipNeedsBlocker(was.mode);
    const double wasStep = 1.0 / (kShaperPathFade * ctx.sampleRate);
    const double driveStep = rampStep(drive, settings.drive, n, ctx.rampShortest);
    const double outputStep = rampStep(output, settings.output, n, ctx.rampShortest);
    const double mixStep = rampStep(mix, settings.mix, n, ctx.rampShortest);
    const double betweenStep = rampStep(between, settings.between, n, ctx.rampShortest);
    // Tone, at the rate it runs at.
    const std::array<BiquadCoefs, 2> shelves = toneShelves(ctx.at(FxParam::ShaperTone), rate);
    for (int i = 0; i < 2; ++i)
        toneCoefs[i].begin(shelves[i], n, ctx.rampShortest);
    const BiquadCoefs through{};
    auto rests = [&](const BiquadRamp &r) {
        return !r.moving && r.now.b0 == through.b0 && r.now.b1 == through.b1 && r.now.a1 == through.a1;
    };
    const bool toned = !rests(toneCoefs[0]) || !rests(toneCoefs[1]);
    if (!toned)
        tones = {};
    const double duckStep = 1.0 / (kShaperDuckSeconds * ctx.sampleRate);
    const bool ducking = wanted != factor || heard < 1.0;
    // Up to its rate a chunk at a time, through the curve, back down, and ducked while its oversampling changes; at
    // every factor alike, written out in the loop rather than through lambdas, which MSVC runs slower here (at 2x, a
    // fifth faster than a loop of its own interleaving up, curve and down sample by sample through two lambdas).
    double buf[2][Upsampler::kMaxSamples];
    for (int ch = 0; ch < 2; ++ch) {
        if (factor == 2)
            for (uint32_t s = 0; s < n; ++s)
                up[ch].processUp(io[ch][s], buf[ch][2 * s], buf[ch][2 * s + 1]);
        else
            ups[factor == 4 ? 0 : 1][ch].process(io[ch], buf[ch], static_cast<int>(n));
    }
    float gains[kChunk];
    for (uint32_t s = 0; s < n; ++s) {
        // One host sample's settings moved on, its curves' fades and the path it left's share (along a raised cosine).
        drive += driveStep;
        output += outputStep;
        mix += mixStep;
        between += betweenStep;
        if (toned) {
            toneCoefs[0].next();
            toneCoefs[1].next();
        }
        if (now.from)
            now.fade = (s + 1) * fadeStep;
        const bool fading = wasFade > 0.0;
        double w = 0.0;
        if (fading) {
            if (wasCurve.from)
                wasCurve.fade = (s + 1) * fadeStep;
            wasFade = std::max(wasFade - wasStep, 0.0);
            w = 0.5 - 0.5 * std::cos(std::numbers::pi * wasFade);
        }
        if (ducking) {
            heard = wanted != factor ? std::max(heard - duckStep, 0.0) : std::min(heard + duckStep, 1.0);
            gains[s] = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * heard));
        }
        // Its samples at the rate through the curve (and the path it left), its Tone around them.
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < factor; ++i) {
                double &x = buf[ch][s * factor + i];
                if (toned)
                    x = tones[ch][0].process(x, toneCoefs[0].now);
                double y = sides[ch].run(now, x, drive, output, mix, block, pole, between);
                if (fading)
                    y = y * (1.0 - w) + w * wasSides[ch].run(wasCurve, x, drive, output, mix, wasBlock, pole, between);
                if (toned)
                    y = tones[ch][1].process(y, toneCoefs[1].now);
                x = y;
            }
    }
    for (int ch = 0; ch < 2; ++ch) {
        if (factor == 2)
            for (uint32_t s = 0; s < n; ++s)
                io[ch][s] = static_cast<float>(down[ch].process(buf[ch][2 * s], buf[ch][2 * s + 1]));
        else { // the decimator adds into what it is given (as the voices' sum into their mix)
            std::fill_n(io[ch], n, 0.0f);
            downs[factor == 4 ? 0 : 1][ch].process(buf[ch], io[ch], static_cast<int>(n));
        }
        if (ducking)
            for (uint32_t s = 0; s < n; ++s)
                io[ch][s] *= gains[s];
    }
    if (heard <= 0.0 && wanted != factor) { // ducked out: the new oversampling, from silence
        factor = wanted;
        sides = {};
        wasSides = {};
        wasFade = 0.0;
        tones = {};
        for (int ch = 0; ch < 2; ++ch) {
            up[ch].reset();
            down[ch].reset();
            for (int k = 0; k < 2; ++k) {
                ups[k][ch].reset();
                downs[k][ch].reset();
            }
        }
    }
    for (int i = 0; i < 2; ++i)
        toneCoefs[i].end();
}

} // namespace fmvibe::fx
