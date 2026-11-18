#pragma once

#include "common.h"

#include <limits>

namespace fmvibe {

// The phaser's all-pass corner (Hz) with its LFO at `lfo` (-1..1), from its params.
double phaserCorner(double centerNorm, double depth, double lfo);
// Its magnitude response, for drawing, with its corner at `cornerHz`, at `polarity` (see sweepPolarity()). At 50 %
// mix its notches go deepest.
double phaserResponse(double hz, double cornerHz, int stages, double feedback, double mix, double polarity);

namespace fx {

// The phaser: first-order all-passes in a row, per side, their corners swept by the LFO; the right side's
// LFO runs a quarter cycle ahead. Those past Stages wait cleared, so more start clean.
struct Phaser {
    std::array<std::array<double, kMaxPhaserStages>, 2> stages{};
    std::array<double, 2> last{}; // the last stage's output, fed back to the first
    double phase = 0.0;           // the LFO, in cycles
    double mix = 0.5;
    double polarity = 1.0; // sweepPolarity()'s, moved there over 10 ms (kPolaritySeconds)
    // The all-passes' coefficient per side, worked out every kSweepStep samples and moved along in a straight
    // line between (see run()): where it was last worked out, and whether it has been yet.
    std::array<double, 2> coef{};
    bool coefKnown = false;
    // Center (in octaves, its frequency's log2) and Depth, ramping to the chunk's as a gain does (see rampStep()):
    // taken as they are once a chunk, modulation would step the sweep's middle every control block. NaN until the
    // first chunk, which takes them as they are.
    double centerOctaves = std::numeric_limits<double>::quiet_NaN();
    double depth = 0.0;

    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its feedback keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
    // Its sweep going round `cycles` while the effects go by idle, worked out again where it is then.
    void idle(double cycles);
};

} // namespace fx
} // namespace fmvibe
