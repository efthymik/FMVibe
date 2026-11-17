// The routing graph's labels, the node under the mouse and what is heard through it now, for the View; what edits the
// routes is RoutingModel's (routingmodel.cpp).

#include "view.h"

#include "fxgraph.h"
#include "layout.h"
#include "routing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

int View::rerouting() const {
    return (drag_.kind == DragKind::Reroute || drag_.kind == DragKind::RerouteFrom) && drag_.moved ? drag_.widget.param
                                                                                                    : -1;
}

// Whether a label's chip `r` comes within 2 px of any of the labels `taken`.
static bool meetsLabel(Rect r, const std::vector<Rect> &taken) {
    constexpr float kGap = 2.0f;
    for (const Rect &other : taken)
        if (r.x < other.right() + kGap && other.x < r.right() + kGap && r.y < other.bottom() + kGap &&
            other.y < r.bottom() + kGap)
            return true;
    return false;
}

Point View::labelAlong(Point start, Point control, Point end, Point middle, float width,
                       const std::vector<Rect> &taken) const {
    constexpr float kClear = kSelectedRing + 1.0f; // clear of a selected node's ring too
    constexpr float kStep = 0.035f;                // along the curve, either way from its middle
    constexpr int kSteps = 8;                      // and no further than a fifth of it from either end
    const std::vector<int> nodes = routing_.enabledNodes();
    std::vector<Point> centers;
    centers.reserve(nodes.size());
    for (int node : nodes)
        centers.push_back(routing_.nodeCenter(node));
    auto clear = [&](Point p, bool ofLabels) {
        const Rect r = chip(p, width);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (overNode(nodes[i], centers[i], r, kClear))
                return false;
        return !ofLabels || !meetsLabel(r, taken);
    };
    const Rect inside{kRouting.x + 0.5f * width + 4.0f, kRouting.y + 12.0f, kRouting.w - width - 8.0f,
                      kRouting.h - 24.0f}; // where its center keeps the chip in the panel, as wireInside() has it
    for (const bool ofLabels : {true, false}) {
        if (clear(middle, ofLabels))
            return middle;
        for (int k = 1; k <= kSteps; ++k)
            for (const float side : {-1.0f, 1.0f}) {
                const float t = 0.5f + side * kStep * static_cast<float>(k), s = 1.0f - t;
                const Point p{s * s * start.x + 2.0f * s * t * control.x + t * t * end.x,
                              s * s * start.y + 2.0f * s * t * control.y + t * t * end.y};
                if (inside.contains(p) && clear(p, ofLabels))
                    return p;
            }
    }
    return middle;
}

std::vector<std::pair<int, Rect>> View::routeLabels() const {
    const std::vector<int> on = routing_.enabledNodes();
    const int moving = rerouting();
    std::vector<std::pair<int, Rect>> labels;
    std::vector<Rect> taken; // each at rest, a mode's glyph widening it about its middle (see labelPill())
    struct Between {
        int param;
        Wire wire;
        float width;
    };
    std::vector<Between> wires;
    for (int from : on)
        for (int to : on) {
            const int param = routeParam(from, to);
            if (from == to || !routing_.wired(param) || param == moving)
                continue;
            const int mode = routeModeParam(from, to);
            const bool glyph = mode >= 0 && routeMode(value(mode)) != RouteMode::Phase;
            wires.push_back(
                {param,
                 wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param)),
                 glyph ? 38.0f + 18.0f : 38.0f});
        }
    // A loop's label goes beside it on the right, or else on its left, above it or at an upper corner: the first
    // of these clear of the other nodes, the loops' labels before it and where the wires' labels would rest with
    // no other label about, inside the panel. Where none is, the first clear of the loops' labels alone; where none
    // is, on the right.
    std::vector<Rect> wished;
    for (const Between &b : wires)
        wished.push_back(chip(labelAlong(b.wire.start, b.wire.control, b.wire.end, b.wire.label, b.width), b.width));
    constexpr float kLoopLabelW = 44.0f;
    const Rect inside{kRouting.x + 4.0f, kRouting.y + 3.0f, kRouting.w - 8.0f, kRouting.h - 6.0f};
    for (int node : on)
        if (const int param = routeParam(node, node); routing_.wired(param) && param != moving) {
            const Point loop = loopCenter(node, routing_.nodeCenter(node));
            const Point offsets[] = {{32.0f, 0.0f}, {-32.0f, 0.0f}, {0.0f, -21.0f}, {32.0f, -20.0f}, {-32.0f, -20.0f}};
            auto clearOf = [&](Rect r, bool all) {
                if (!inside.contains({r.x, r.y}) || !inside.contains({r.right(), r.bottom()}) || meetsLabel(r, taken))
                    return false;
                if (!all)
                    return true;
                for (int other : on)
                    if (other != node && overNode(other, routing_.nodeCenter(other), r, 1.0f))
                        return false;
                return !meetsLabel(r, wished);
            };
            Rect idle = chip({loop.x + offsets[0].x, loop.y + offsets[0].y}, kLoopLabelW);
            bool found = false;
            for (const bool all : {true, false})
                for (const Point o : offsets)
                    if (const Rect r = chip({loop.x + o.x, loop.y + o.y}, kLoopLabelW); !found && clearOf(r, all)) {
                        idle = r;
                        found = true;
                    }
            labels.emplace_back(param, idle);
            taken.push_back(idle);
        }
    for (const Between &b : wires) {
        const Point at = labelAlong(b.wire.start, b.wire.control, b.wire.end, b.wire.label, b.width, taken);
        labels.emplace_back(b.param, chip(at, 38.0f));
        taken.push_back(chip(at, b.width));
    }
    for (int node : on)
        if (routing_.goesOut(node) && outParam(node) != moving) {
            const OutWire w = outWireOf(node, routing_.nodeCenter(node), routing_.wireBend(outParam(node)));
            // Beside the line where it is too short for the label to fit along it; else along it, clear of the nodes
            // and the labels before it.
            const Point at = w.beside ? w.label : labelAlong(w.start, w.control, w.end, w.label, 40.0f, taken);
            labels.emplace_back(outParam(node), chip(at, 40.0f));
            taken.push_back(chip(at, 40.0f));
        }
    return labels;
}

bool View::reachesOut(int node) const {
    // A search along the routes that are open, from `node` through switched-on nodes to one that goes out.
    std::array<bool, kNodes> visited{};
    auto reaches = [&](auto &self, int from) -> bool {
        if (visited[from] || !routing_.nodeOn(from))
            return false;
        visited[from] = true;
        if (liveAmount(outParam(from)) > 0.0)
            return true;
        for (int to = 0; to < kNodes; ++to)
            if (to != from && liveAmount(routeParam(from, to)) > 0.0 && self(self, to))
                return true;
        return false;
    };
    return reaches(reaches, node);
}

double View::outLevel(int node) const {
    if (!routing_.nodeOn(node))
        return 0.0;
    double level = liveAmount(outParam(node));
    for (int to = 0; to < kNodes; ++to)
        if (to != node && liveAmount(routeParam(node, to)) > level && reachesOut(to))
            level = liveAmount(routeParam(node, to));
    return level;
}

int View::nodeAt(Point p) const {
    for (int node = kNodes - 1; node >= 0; --node) {
        if (!routing_.nodeOn(node))
            continue;
        const Point c = routing_.nodeCenter(node);
        const bool inside = isBoxNode(node)
                                ? std::abs(p.x - c.x) <= kFilterHalfW + 4.0f && std::abs(p.y - c.y) <= kFilterHalfH + 4.0f
                                : std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius + 4.0f;
        if (inside)
            return node;
    }
    return -1;
}


void detail::paintGridMarks(Canvas &c, Rect area, int cols, int rows, const Point *target) {
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col) {
            const Point p = gridPoint(area, cols, rows, col, row);
            if (target && std::hypot(p.x - target->x, p.y - target->y) < 0.5f)
                continue; // drawn lit below
            c.line({p.x - 3.5f, p.y}, {p.x + 3.5f, p.y}, kFaint.alpha(0.45f), 1.0f);
            c.line({p.x, p.y - 3.5f}, {p.x, p.y + 3.5f}, kFaint.alpha(0.45f), 1.0f);
        }
    if (target) {
        c.line({target->x - 7.0f, target->y}, {target->x + 7.0f, target->y}, kAccent, 1.6f);
        c.line({target->x, target->y - 7.0f}, {target->x, target->y + 7.0f}, kAccent, 1.6f);
    }
}

// --- The routing graph's grid (Ctrl+drag)

bool View::setCtrl(bool held) {
    if (held == ctrlHeld_)
        return false;
    ctrlHeld_ = held;
    return true; // the grid's points show or go, and a node being dragged snaps or goes free
}

bool View::routingGridShown() const {
    return ctrlHeld_ && controller_.layout().view == RoutingView::Graph && shapeEditing() < 0 &&
           partialEditing() < 0 && (drag_.kind == DragKind::Node || (drag_.kind == DragKind::None && kRouting.contains(hoverPoint_)));
}

// The grid's points nearest `want` first; a point a node fits at, clear of the others, takes it.
namespace {
template <class Fits>
Point nearestGridPoint(Rect area, int cols, int rows, Point want, Fits fits, Point otherwise) {
    std::vector<Point> points;
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col)
            points.push_back(gridPoint(area, cols, rows, col, row));
    std::sort(points.begin(), points.end(), [&](Point a, Point b) {
        return std::hypot(a.x - want.x, a.y - want.y) < std::hypot(b.x - want.x, b.y - want.y);
    });
    for (const Point p : points)
        if (fits(p))
            return p;
    return otherwise;
}
} // namespace

Point View::routingGridPoint(int node, Point want) const {
    // Where RoutingModel::nodePlace() would leave it untouched, nothing is in its way.
    auto fits = [&](Point p) {
        bool found = false;
        const Point at = routing_.nodePlace(node, p, &found);
        return found && std::hypot(at.x - p.x, at.y - p.y) < 0.5f;
    };
    return nearestGridPoint(kGraph, kRoutingGridCols, kRoutingGridRows, want, fits, routing_.nodePlace(node, want));
}

} // namespace fmvibe::gui
