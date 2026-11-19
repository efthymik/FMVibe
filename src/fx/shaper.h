#pragma once

#include "common.h"

#include "../clipcurve.h"
#include "../oversampler.h"

namespace fmvibe::fx {

// How far the shaper's Tone tilts at its ends (dB above its corner) and where its corner is (Hz).
constexpr double kShaperToneDb = 15.0;
constexpr double kShaperToneHz = 700.0;
// How long the shaper ducks out and back in (each way) as its oversampling changes.
constexpr double kShaperDuckSeconds = 0.003;

// The shaper effect: the voices' shapers' curves on the whole mix, at twice the rate (see run()), or four or eight
// times as its Oversampling says.
struct Shaper {
    std::array<ClipState, 2> sides{};
    double drive = 1.0; // gains, ramped
    double output = 1.0;
    double mix = 1.0;
    double between = 1.0; // after each Stack stage but the last
    // A shaper's curve as a chunk ran it: what its path (see run()) is built from.
    struct Path {
        ClipMode mode = ClipMode::Soft;
        bool symmetric = false;
        int stack = 1;
    };
    // The drawn curve as a table (see run()).
    ClipTable curve;
    // Up to twice the rate and back down, a side each, designed in prepare(); and whether the last chunk ran a Stack
    // curve.
    std::array<HalfBand, 2> up, down;
    bool stacked = false;
    // Whether the last chunk ran the shaper (start() clears it), and its curve then.
    bool ran = false;
    Path last;
    // For a moment after its curve goes to Stack or leaves it, the path it left, fading out as the new one fades
    // in, with its curve and its sides as they were. Both run on the same samples at twice the rate.
    Path was;
    std::array<ClipState, 2> wasSides{};
    double wasFade = 0.0; // 1 .. 0
    // At four and eight times the rate: up and back down through the voices' oversampling's stages, a side each,
    // designed in prepare() for both (index 0 at 4x, 1 at 8x), as a path beside it is padded (see fxlag.h).
    std::array<std::array<Upsampler, 2>, 2> ups;
    std::array<std::array<Decimator, 2>, 2> downs;
    int factor = 0;      // the oversampling it runs at (0 until its first chunk, which takes it as it is)
    double heard = 1.0;  // ducking out to change its oversampling, and back in
    // Tone: a high shelf before the curve and the opposite one after, a side each, ramping from letting everything
    // through (as they rest at 0, where they don't run).
    std::array<std::array<Biquad, 2>, 2> tones{}; // per side: before, after
    std::array<BiquadRamp, 2> toneCoefs{};

    void prepare(double sampleRate);
    // Clears it as it is about to fade in, and jumps its ramps to the current settings.
    void start(const Context &ctx);
    void run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fmvibe::fx
