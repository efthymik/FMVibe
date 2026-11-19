#pragma once

#include "common.h"

namespace fmvibe {

// Where the phase disperser centers its all-passes (Hz): Freq (0..1 on the cutoff curve), moved by Key Track (0..1)
// with `pitch` (semitones, 69 is A4) from C4 on, as the filter's key tracking moves its cutoff.
double disperserCenter(double freqNorm, double keyTrack, double pitch);
// How many samples each all-pass's delays wait at Stretch `stretch` (1..kMaxDisperserStretch): as many at 48 kHz, and
// as long a time at other rates, so its dispersion repeats every 48 kHz / Stretch at any rate.
int disperserStretch(double stretch, double sampleRate);
// The phase disperser's all-pass (b0, b1, b2, a1, a2), its delays `stretch` samples each, putting its dispersion at
// `hz` (kept under the half rate) and, stretched, at each copy of it every sampleRate / stretch: designed where
// stretch times `hz` folds back into the band, Pinch setting its Q from 0.3 to 6. And how long the sound at `at` Hz
// comes out after the rest through `stages` of them (seconds).
std::array<double, 5> disperserCoefs(double hz, double pinch, double sampleRate, int stretch = 1);
double disperserDelay(double at, double hz, double pinch, int stages, double sampleRate, int stretch = 1);

namespace fx {

// How long the phase disperser takes to fade from one stage count to another (see Disperser::run()); and as its Stretch
// changes, to fade out to what goes in, and back in for each sample of the new stretch (at 48 kHz, as long a time at
// other rates), from kDisperserMinFadeInSeconds up to kDisperserMaxFadeInSeconds: a stretched chain starting from
// silence buzzes at the rate over its stretch for as long as its delays are long, which a slower fade keeps down.
constexpr double kDisperserFadeSeconds = 0.01;
constexpr double kDisperserStretchFadeSeconds = 0.005;
constexpr double kDisperserFadeInSeconds = 0.004;
constexpr double kDisperserMinFadeInSeconds = 0.02;
constexpr double kDisperserMaxFadeInSeconds = 0.25;

// The phase disperser's all-passes, per side; those past what it plays wait cleared, so more start clean. Its
// coefficients ramp from one chunk's to the next (see BiquadRamp), and a new Stages fades in from the count it played
// over kDisperserFadeSeconds: the chain's output after either count comes out of the same run, so both play at once
// for no more than the longer chain costs. Another change while it fades waits for the fade to end.
//
// Stretched, each all-pass's two delays wait `stretch` samples rather than one: its response repeats every
// sampleRate / stretch, so the chirp comes again across the spectrum, each copy as long as one at Freq. The delays are
// rings in `rings`, allocated in prepare() for the most it can stretch at the rate, each place along them holding
// every all-pass's two delays (kRingStride values), so a short stretch only touches the buffer's first pages. A new
// Stretch fades the effect out to what goes in over kDisperserStretchFadeSeconds, takes the new length with its
// all-passes cleared, and fades back in, the slower the longer it is: an all-pass can't go from one length to another
// without a jump.
struct Disperser {
    static constexpr int kRingStride = 2 * kMaxDisperserStages * 2; // per side, per stage, two delays
    std::array<std::array<Biquad, kMaxDisperserStages>, 2> stages{}; // unstretched
    BasicZeroBuffer<double> rings; // stretched (see above)
    int ringLength = 0;            // the longest stretch at the rate prepare() was given
    int ringAt = 0;                // the place along the rings this sample reads and writes
    int ringStages = 0;            // how many stages along the rings may hold something (those past are silent)
    BiquadRamp coefs;
    double mix = 1.0;
    int playing = 0; // the stage count it plays (0 until its first chunk, which takes Stages as it is)
    int fadingFrom = 0; // the count it fades out from, while `fade` is above 0
    double fade = 0.0;  // 1 down to 0 over kDisperserFadeSeconds
    int stretch = 0;     // the stretch it plays (0 until its first chunk, which takes Stretch as it is)
    double heard = 1.0;  // how much of its sound is heard, falling to 0 and back as Stretch changes

    // Allocates the rings. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its ring keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);

  private:
    void clear(); // every all-pass, stretched or not, back to silence
};

} // namespace fx
} // namespace fmvibe
