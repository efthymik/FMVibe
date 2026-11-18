#pragma once

#include "common.h"

namespace fmvibe {

// The phase disperser's all-pass (b0, b1, b2, a1, a2) at Freq (0..1 on the cutoff curve), Pinch setting its Q
// from 0.3 to 6, and how long the sound at `hz` comes out after the rest through `stages` of them (seconds).
std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate);
double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate);

namespace fx {

// How long the phase disperser takes to fade from one stage count to another (see Disperser::run()).
constexpr double kDisperserFadeSeconds = 0.01;

// The phase disperser's all-passes, per side; those past what it plays wait cleared, so more start clean. Its
// coefficients ramp from one chunk's to the next (see BiquadRamp), and a new Stages fades in from the count it played
// over kDisperserFadeSeconds: the chain's output after either count comes out of the same run, so both play at once
// for no more than the longer chain costs. Another change while it fades waits for the fade to end.
struct Disperser {
    std::array<std::array<Biquad, kMaxDisperserStages>, 2> stages{};
    BiquadRamp coefs;
    double mix = 1.0;
    int playing = 0; // the stage count it plays (0 until its first chunk, which takes Stages as it is)
    int fadingFrom = 0; // the count it fades out from, while `fade` is above 0
    double fade = 0.0;  // 1 down to 0 over kDisperserFadeSeconds

    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its ring keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
