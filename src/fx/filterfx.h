#pragma once

#include "common.h"

#include "../filter.h"
#include "../oversampler.h"

#include <limits>

namespace fmvibe::fx {

// The filter effect: a voice filter a side. Run at twice the rate (see run()), as the voices' filters run
// oversampled: up through `up` and back down through `down`, a side each, designed in prepare().
struct FilterFx {
    std::array<Filter, 2> channels{};
    std::array<ZeroBuffer, 2> combs; // comb delay lines, combLength() long at twice the rate
    std::array<HalfBand, 2> up, down;
    double mix = 1.0;
    // Its settings, and the params (and pitch, while it tracks the key) they were worked out from: NaN until the
    // first chunk, so worked out again only when one moves (see run()).
    FilterSettings settings;
    FilterInputs settingsFor{std::numeric_limits<double>::quiet_NaN()};
    // Those inputs as they ramp to the chunk's, the continuous ones as a gain does (see rampStep()), so modulation
    // doesn't step the filter every control block: NaN until the first chunk, which takes them as they are. And the
    // chunk's ramp: where they go from and to, how far each moves a sample, whether they get there, whether they move
    // at all, the chunk's length, and what the settings scale the outputs by at the step's start and end (see
    // FilterGains), and the step's length.
    FilterInputs inputsNow{std::numeric_limits<double>::quiet_NaN()};
    struct Ramp {
        FilterInputs from{}, to{}, step{};
        bool arrives = true, moving = false;
        bool movedLast = false; // whether the last chunk's moved, leaving the outputs scaled by gainsTo
        uint32_t n = 0, length = 1;
        FilterGains gainsFrom{}, gainsTo{};
    } ramp;
    double rate = 96000.0; // the rate it runs at, twice the host's
    // Filter FM (see Context::cutoffOctaves): how far it can take the filter at twice the rate, and where the last
    // sample left the cutoff, in octaves.
    CutoffFmRange fmRange;
    double fmLast = 0.0;

    // Allocates the comb lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample its filters put out, so a resonant comb keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);

private:
    // Its settings from its inputs (see filterSettingsFrom()), taken up when they differ from those last taken.
    void settle(const FilterInputs &inputs);
    // The ramp at the chunk's sample `s`: its settings worked out anew at a step's start (returning true), what they
    // scale the outputs by glided on (see Ramp).
    bool rampStepAt(uint32_t s);
    // run() with an operator moving the cutoff: a loop of its own, so the plain one stays as it is.
    double runModulated(const float *octaves, float *const *io, uint32_t n, double mixStep);
};

} // namespace fmvibe::fx
