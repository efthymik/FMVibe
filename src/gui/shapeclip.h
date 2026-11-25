#pragma once

// A drawn shape copied (Copy Shape, or Ctrl+C over a shape) to paste into any drawn shape: a Curve source's, an
// operator's envelope, a shaper's curve, the note source's or velocity's curve, or a sequencer lane's (see ShapeEditor
// and SeqPanel). It holds the shape as it is drawn, whatever it was drawn in: its points along it (0..1), their levels
// (-1 at the bottom to 1 at the top), each one's bend of the segment after it (see shapeBend()), whether it runs on
// round its end to its first point, and its loop region, if it has one. And how a paste fits it to where it goes.

#include "../params.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fmvibe::gui {

struct ShapeClip {
    struct Point {
        double x, y, curve;
    };
    std::vector<Point> points; // in order along the shape, the first at 0
    bool wraps = false;        // its last segment goes on round its end to the first point (an LFO's, a lane's)
    bool looped = false;       // with a loop region (a Curve source's, an operator's envelope's)
    double loopStart = 0.0, loopEnd = 1.0;
    bool empty() const { return points.size() < 2; }
};

// The one the editor copies into and pastes from (each plugin's own, shared by its editors).
inline ShapeClip &shapeClipboard() {
    static ShapeClip clip;
    return clip;
}

// Where a shape that doesn't wrap stands at `x`: on the segment it falls in, bent as that segment is.
inline double clipLevel(const std::vector<ShapeClip::Point> &p, double x) {
    if (p.empty())
        return 0.0;
    if (p.size() == 1)
        return p[0].y;
    size_t k = 0;
    while (k + 2 < p.size() && p[k + 1].x <= x)
        ++k;
    const double span = p[k + 1].x - p[k].x;
    const double t = span > 1e-12 ? std::clamp((x - p[k].x) / span, 0.0, 1.0) : 1.0;
    return p[k].y + (p[k + 1].y - p[k].y) * shapeBend(t, p[k].curve);
}

// A point put on the shape at `x`, splitting the segment it falls in, each part bent as much as keeps it near the same
// curve (as a widened card's shift does), unless one is there already: where it is.
inline size_t splitClipAt(std::vector<ShapeClip::Point> &p, double x) {
    constexpr double kSame = 1e-4;
    for (size_t k = 0; k < p.size(); ++k)
        if (std::abs(p[k].x - x) < kSame)
            return k;
    size_t k = 0;
    while (k + 1 < p.size() && p[k + 1].x <= x)
        ++k;
    if (k + 1 >= p.size()) { // past the last point: the shape holds its level to there
        p.push_back({x, p.back().y, 0.0});
        return p.size() - 1;
    }
    const ShapeClip::Point from = p[k];
    const double span = p[k + 1].x - from.x;
    const double u = span > 1e-12 ? (x - from.x) / span : 0.0;
    const ShapeClip::Point split{x, from.y + (p[k + 1].y - from.y) * shapeBend(u, from.curve), from.curve * (1.0 - u)};
    p[k].curve = from.curve * u;
    p.insert(p.begin() + static_cast<std::ptrdiff_t>(k + 1), split);
    return k + 1;
}

// For a shape that wraps, a point at its start: where its way round from the last point to the first passes 0.
inline void clipFromStart(std::vector<ShapeClip::Point> &p) {
    if (p.empty() || p.front().x <= 0.0)
        return;
    const ShapeClip::Point last = p.back(), first = p.front();
    const double span = 1.0 - last.x + first.x;
    const double u = span > 1e-12 ? (1.0 - last.x) / span : 0.0;
    p.back().curve = last.curve * u;
    p.insert(p.begin(), {0.0, last.y + (first.y - last.y) * shapeBend(u, last.curve), last.curve * (1.0 - u)});
}

// Its points from its start to its end, for a shape that doesn't wrap: the first at 0 and the last at 1, a wrapping
// shape's way back round to its first point becoming a point at the end, at the first's level.
inline std::vector<ShapeClip::Point> clipToEnds(const ShapeClip &clip) {
    std::vector<ShapeClip::Point> p = clip.points;
    if (p.empty())
        return p;
    p.front().x = 0.0;
    if (clip.wraps && p.back().x < 1.0 - 1e-9)
        p.push_back({1.0, p.front().y, 0.0});
    else
        p.back().x = 1.0;
    return p;
}

// Thinned to `most` points at the most, keeping its ends: one at a time, the point whose going changes the shape
// least (the segment left across its place bent as near it as it can be).
inline std::vector<ShapeClip::Point> thinClip(std::vector<ShapeClip::Point> p, size_t most) {
    if (p.size() <= most || p.size() <= 2)
        return p;
    constexpr int kSamples = 256;
    std::vector<double> want(kSamples + 1);
    for (int i = 0; i <= kSamples; ++i)
        want[static_cast<size_t>(i)] = clipLevel(p, static_cast<double>(i) / kSamples);
    while (p.size() > most && p.size() > 2) {
        size_t best = 1;
        double bestCurve = p[0].curve, bestError = std::numeric_limits<double>::infinity();
        for (size_t k = 1; k + 1 < p.size(); ++k) {
            const ShapeClip::Point &a = p[k - 1], &b = p[k + 1];
            const int i0 = static_cast<int>(std::floor(a.x * kSamples)), i1 = static_cast<int>(std::ceil(b.x * kSamples));
            for (int c = -8; c <= 8; ++c) { // the bend across the gap
                const double curve = c / 8.0;
                double error = 0.0;
                for (int i = std::max(i0, 0); i <= std::min(i1, kSamples); ++i) {
                    const double x = static_cast<double>(i) / kSamples;
                    const double span = b.x - a.x;
                    const double t = span > 1e-12 ? std::clamp((x - a.x) / span, 0.0, 1.0) : 1.0;
                    const double y = a.y + (b.y - a.y) * shapeBend(t, curve);
                    error = std::max(error, std::abs(y - want[static_cast<size_t>(i)]));
                }
                if (error < bestError) {
                    bestError = error;
                    best = k;
                    bestCurve = curve;
                }
            }
        }
        p[best - 1].curve = bestCurve;
        p.erase(p.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return p;
}

} // namespace fmvibe::gui
