#pragma once

#include "common.h"

namespace fmvibe::fx {

// The compressor. The gain reduction (dB, 0 or below) follows the static curve at the attack
// or release pace.
struct Compressor {
    double reduction = 0.0;
    double makeup = 1.0; // gains, ramped
    double mix = 1.0;
    // The high-pass on what the detector hears (Detector HP), a side each: ramping from letting everything through
    // (as it rests at Off, where it doesn't run) to its cut, so switching it on doesn't jump.
    std::array<Biquad, 2> detector{};
    BiquadRamp detectorCoefs{};

    void start(const Context &ctx);
    // Returns whether its gain is still on its way to where the curve puts it (a release running out). Its reduction
    // goes to the meters' band 0.
    bool run(const Context &ctx, float *const *io, uint32_t n, const Meters &meters);
};

} // namespace fmvibe::fx
