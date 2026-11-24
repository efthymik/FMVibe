#pragma once

#include "params.h"

#include <algorithm>
#include <cmath>

namespace fmvibe {

// An operator's drawn envelope (see OpParam::EnvMode): its level, 0..1, along a shape of points from the start of its
// time to its end, a sample at a time. Its points are a freeform shape's (see ShapeParam), as a curve has them: the
// first at the start, the last at the end (whatever it stores), y -1..1 for a level of 0..1, each segment bent by its
// first point's curve (see shapeBend()). The segment it is in is stepped, not looked up: a multiply a sample along a bent
// one, a few exps as it enters another.
//
// From each note it plays its shape from the start. In Sustain, while the key is down, it loops its loop region; once
// it is let go (in Sustain at once, wherever it is, from the loop's end on), it plays on to the end, carrying the
// level it was at as an offset on top of the shape that fades out by the end of the segment it goes on along (or over
// fadeSeconds where there is none: the loop ending at the shape's end, or on a point the shape jumps from), so it
// goes on from the level it was at. One-Shot plays the shape once and holds its end level while the key is down. Its
// end reached once let go, it fades out over fadeSeconds, and stands idle: an operator that is done. A note starting
// it again, or a first note on a shape that starts above nothing, starts from the level it was at, the same way.
//
// start() and release() only note what happened: prepare(), before each block's samples, takes them up, as it has the
// shape and its time then.
class DrawnEnvelope {
public:
    static constexpr double kFadeSeconds = 0.005;

    void start() {
        startAsked_ = true;
        releaseAsked_ = false;
        held_ = true;
    }
    void release() {
        if (held_)
            releaseAsked_ = true;
        held_ = false;
    }
    // Stops at once, keeping what start() asked if it hasn't been taken up (a note held as the editor switches the
    // operator back to its AHDSR: switched back again, it starts from where the operator is then).
    void stop() {
        stage_ = Stage::Idle;
        level_ = 0.0;
        startAsked_ = held_;
        releaseAsked_ = false;
    }

    bool idle() const { return stage_ == Stage::Idle && !startAsked_; }
    bool held() const { return held_; }
    double level() const { return level_; }
    // Where it is along its shape, 0..1, for the editor; -1 while idle.
    double phase() const { return stage_ == Stage::Idle ? -1.0 : x_; }

    // The shape (its point count, then its points), how many samples the whole of it lasts, its loop region (0..1,
    // start no later than end) and whether it loops it while held; and `from`, the level an operator is at, which a
    // start goes on from.
    void prepare(const double *shape, double samples, double loopStart, double loopEnd, bool sustain, double from,
                 double sampleRate) {
        shape_ = shape;
        count_ = shapePointCount(shape[0]);
        samples_ = std::max(samples, 1.0);
        // Its segment found again where it is, as the shape or its time may have changed since the last block.
        if (stage_ == Stage::Play && !seek(x_)) {
            x_ = 1.0;
            reachEnd();
        }
        loopStart_ = loopStart;
        loopEnd_ = loopEnd;
        sustain_ = sustain;
        fadeStep_ = 1.0 / std::max(kFadeSeconds * sampleRate, 1.0);
        if (startAsked_) {
            startAsked_ = false;
            level_ = from;
            stage_ = Stage::Play;
            goOnFrom(0.0, from);
        }
        if (releaseAsked_) {
            releaseAsked_ = false;
            if (stage_ == Stage::Hold)
                fadeOut();
            else if (stage_ == Stage::Play && sustain_ && x_ < loopEnd_)
                goOnFrom(loopEnd_, level_); // leaves the loop at once
        }
    }

    double next() {
        switch (stage_) {
        case Stage::Play:
            return play();
        case Stage::Fade:
            level_ -= fadeFrom_ * fadeStep_;
            if (level_ <= 0.0)
                stop();
            return level_;
        case Stage::Hold:
        case Stage::Idle:
            break;
        }
        return level_;
    }

private:
    enum class Stage { Idle, Play, Hold, Fade };

    double pointX(int point) const {
        return point <= 0 ? 0.0 : point >= count_ - 1 ? 1.0 : shape_[shapeFieldIndex(0, point, PointField::X)];
    }
    double pointLevel(int point) const {
        return 0.5 * (std::clamp(shape_[shapeFieldIndex(0, point, PointField::Y)], -1.0, 1.0) + 1.0);
    }

    // Into segment `seg` (from point seg to seg + 1), `t` of the way along it.
    void enter(int seg, double t) {
        seg_ = seg;
        x0_ = pointX(seg);
        span_ = std::max(pointX(seg + 1) - x0_, 0.0);
        y0_ = pointLevel(seg);
        rise_ = pointLevel(seg + 1) - y0_;
        t_ = t;
        dt_ = span_ > kShapeSameTime ? 1.0 / (span_ * samples_) : 1.0;
        const double curve = shape_[shapeFieldIndex(0, seg, PointField::Curve)];
        bent_ = std::abs(curve) >= 1e-3;
        if (bent_) {
            const double a = 8.0 * curve;
            mul_ = std::exp(a * dt_);
            inv_ = 1.0 / std::expm1(a);
            e_ = std::exp(a * t);
        }
    }
    // Into the segment at `x` (the last point at or before it starts it, as shapeValue() has it), or past the end.
    bool seek(double x) {
        if (x >= 1.0 || count_ < 2)
            return false;
        int seg = 0;
        while (seg + 2 < count_ && pointX(seg + 1) <= x)
            ++seg;
        const double from = pointX(seg), span = pointX(seg + 1) - from;
        enter(seg, span > kShapeSameTime ? std::clamp((x - from) / span, 0.0, 1.0) : 1.0);
        x_ = x;
        return true;
    }
    double shapeNow() const { return y0_ + rise_ * (bent_ ? (e_ - 1.0) * inv_ : t_); }
    double carried() const { // the offset where it is now
        if (offset_ == 0.0)
            return 0.0;
        if (glide_ >= 0.0)
            return offset_ * glide_;
        return offset_ * std::clamp((offsetTo_ - x_) / (offsetTo_ - offsetFrom_), 0.0, 1.0);
    }

    // On from `x`, where the level is `level`: what it is above or below the shape there carried on top, fading out by
    // the end of the segment it goes on along (see DrawnEnvelope), or over kFadeSeconds where that is no time at all.
    void goOnFrom(double x, double level) {
        offset_ = 0.0;
        glide_ = -1.0;
        if (!seek(x)) { // the shape's end
            x_ = 1.0;
            level_ = level;
            reachEnd();
            return;
        }
        offset_ = level - shapeNow();
        offsetFrom_ = x;
        offsetTo_ = x0_ + span_;
        // Where the shape jumps straight up or down at x (points at its time), it goes with the jump, gliding there.
        int at = 0;
        for (int point = 0; point < count_; ++point)
            at += std::abs(pointX(point) - x) <= kShapeSameTime ? 1 : 0;
        if (at >= 2 || offsetTo_ - offsetFrom_ <= kShapeSameTime)
            glide_ = 1.0;
    }

    void reachEnd() {
        if (held_)
            stage_ = Stage::Hold; // held (One-Shot, or a loop that doesn't come round): at the end, where it stays
        else
            fadeOut();
    }
    void fadeOut() {
        fadeFrom_ = level_;
        stage_ = level_ > 0.0 ? Stage::Fade : Stage::Idle;
        if (stage_ == Stage::Idle)
            stop();
    }

    double play() {
        t_ += dt_;
        if (bent_)
            e_ *= mul_;
        x_ = x0_ + std::min(t_, 1.0) * span_;
        if (glide_ > 0.0)
            glide_ = std::max(glide_ - fadeStep_, 0.0);
        if (held_ && sustain_ && x_ >= loopEnd_) { // round the loop
            const double length = loopEnd_ - loopStart_;
            offset_ = 0.0;
            if (!seek(length > kShapeSameTime ? loopStart_ + std::fmod(x_ - loopEnd_, length) : loopStart_)) {
                reachEnd();
                return level_;
            }
        } else if (t_ >= 1.0 && !seek(x0_ + t_ * span_)) { // into the next segment, past any jump, or the end
            x_ = 1.0;
            level_ = std::clamp(pointLevel(count_ - 1) + carried(), 0.0, 1.0);
            reachEnd();
            return level_;
        }
        level_ = std::clamp(shapeNow() + carried(), 0.0, 1.0);
        return level_;
    }

    const double *shape_ = nullptr;
    int count_ = 2;
    double samples_ = 1.0;
    double loopStart_ = 0.0, loopEnd_ = 1.0;
    bool sustain_ = true;
    double fadeStep_ = 1.0;

    Stage stage_ = Stage::Idle;
    bool held_ = false;
    bool startAsked_ = false;
    bool releaseAsked_ = false;
    double level_ = 0.0;
    double x_ = 0.0;
    // The segment it is in: its index, its start and length along the shape, its first level and its rise, how far
    // along it is (t_) and by how much a sample (dt_), and for a bent one exp(8 curve t) (e_), its step and scale.
    int seg_ = 0;
    double x0_ = 0.0, span_ = 1.0, y0_ = 0.0, rise_ = 0.0;
    double t_ = 0.0, dt_ = 0.0;
    bool bent_ = false;
    double e_ = 1.0, mul_ = 1.0, inv_ = 1.0;
    // What it carries on top of the shape (see goOnFrom()): fading along the shape from offsetFrom_ to offsetTo_, or,
    // with glide_ not below 0, by the share of it left.
    double offset_ = 0.0, offsetFrom_ = 0.0, offsetTo_ = 1.0, glide_ = -1.0;
    double fadeFrom_ = 0.0; // the level its fade out started at
};

} // namespace fmvibe
