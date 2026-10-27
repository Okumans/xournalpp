#pragma once

#include <algorithm>
#include <cmath>

namespace prototype {

struct ScrollDelta {
    double x = 0;
    double y = 0;
};

class ScrollAxisLock {
public:
    enum class Axis { Undecided, Vertical, Horizontal };

    void begin() {
        axis = Axis::Undecided;
        pending = {};
        distanceX = 0;
        distanceY = 0;
    }

    ScrollDelta filter(double dx, double dy, bool horizontalAllowed) {
        if (!horizontalAllowed) {
            axis = Axis::Vertical;
            pending = {};
            return {0, dy};
        }
        if (axis != Axis::Undecided) {
            return apply(dx, dy);
        }

        pending.x += dx;
        pending.y += dy;
        distanceX += std::abs(dx);
        distanceY += std::abs(dy);
        if (distanceX + distanceY < axisLockDistance) {
            return {};
        }

        axis = distanceX > distanceY * horizontalIntentRatio ? Axis::Horizontal : Axis::Vertical;
        const auto accumulated = pending;
        pending = {};
        return apply(accumulated.x, accumulated.y);
    }

    ScrollDelta finish(bool horizontalAllowed) {
        if (axis == Axis::Undecided) {
            axis = Axis::Vertical;
            const auto accumulatedY = pending.y;
            pending = {};
            return horizontalAllowed ? ScrollDelta{0, accumulatedY} : ScrollDelta{};
        }
        pending = {};
        return {};
    }

    ScrollDelta filterVelocity(double vx, double vy, bool horizontalAllowed) const {
        if (!horizontalAllowed || axis != Axis::Horizontal) {
            return {0, vy};
        }
        return {vx * horizontalScrollScale, 0};
    }

    Axis lockedAxis() const { return axis; }

private:
    static constexpr double axisLockDistance = 4.0;
    static constexpr double horizontalIntentRatio = 2.0;
    static constexpr double horizontalScrollScale = 0.18;

    Axis axis = Axis::Undecided;
    ScrollDelta pending;
    double distanceX = 0;
    double distanceY = 0;

    ScrollDelta apply(double dx, double dy) const {
        return axis == Axis::Horizontal ? ScrollDelta{dx * horizontalScrollScale, 0} : ScrollDelta{0, dy};
    }
};

class ScrollMomentum {
public:
    bool start(double vx, double vy) {
        if (!std::isfinite(vx) || !std::isfinite(vy)) {
            stop();
            return false;
        }
        const auto speed = std::hypot(vx, vy);
        if (speed < stopSpeed) {
            stop();
            return false;
        }
        const auto scale = speed > maxSpeed ? maxSpeed / speed : 1.0;
        velocity = {vx * scale, vy * scale};
        lastFrameTime = 0;
        active = true;
        return true;
    }

    bool advance(long long frameTime, ScrollDelta& delta) {
        delta = {};
        if (!active || frameTime <= 0) {
            return active;
        }
        if (lastFrameTime == 0) {
            lastFrameTime = frameTime;
            return true;
        }

        const auto elapsedMs = std::clamp((frameTime - lastFrameTime) / 1000.0, 0.0, 50.0);
        lastFrameTime = frameTime;
        const auto decay = std::exp(-elapsedMs / timeConstantMs);
        const auto distanceScale = timeConstantMs * (1.0 - decay);
        delta = {velocity.x * distanceScale, velocity.y * distanceScale};
        velocity.x *= decay;
        velocity.y *= decay;
        if (std::hypot(velocity.x, velocity.y) < stopSpeed) {
            stop();
        }
        return active;
    }

    void stop() {
        active = false;
        velocity = {};
        lastFrameTime = 0;
    }

    bool isActive() const { return active; }

private:
    static constexpr double maxSpeed = 1.0;
    static constexpr double stopSpeed = 0.015;
    static constexpr double timeConstantMs = 420.0;

    ScrollDelta velocity;
    long long lastFrameTime = 0;
    bool active = false;
};

}  // namespace prototype
