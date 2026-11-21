#pragma once

// A host extension of FMVibe's own, for a host that stands in for a transport and lets the plugin set its tempo:
// fmvibe-uihost offers it. The synth then always plays its own tempo, starting at the host's, and the host's follows
// it as the editor's title bar or a preset sets it; the title bar shows no H (see gui::Controller::hasTempo()). Hosts
// that don't offer it (every DAW) keep their tempo their own. Both calls are made on the main thread; the tempo is in
// BPM.

#include <clap/clap.h>

namespace fmvibe {

inline constexpr char kExtHostTempo[] = "io.github.pcdlin.fmvibe.host-tempo/1";

struct HostTempo {
    double(CLAP_ABI *get)(const clap_host_t *host);
    // The host plays at `bpm` from its next block on, held to its range.
    void(CLAP_ABI *set)(const clap_host_t *host, double bpm);
};

} // namespace fmvibe
