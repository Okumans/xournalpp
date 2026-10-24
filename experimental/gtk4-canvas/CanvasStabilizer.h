#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

#include "CanvasDrawing.h"

namespace prototype {

enum class AveragingMethod { None, Arithmetic, VelocityGaussian };
enum class StrokePreprocessor { None, Deadzone, Inertia };

struct StabilizerConfig {
    AveragingMethod averaging = AveragingMethod::None;
    StrokePreprocessor preprocessor = StrokePreprocessor::None;
    size_t bufferSize = 20;
    double sigma = 0.5;
    double deadzoneRadius = 1.3;
    bool cuspDetection = true;
    double drag = 0.4;
    double mass = 5;
    bool finalizeStroke = true;
};

struct StabilizerSample {
    double x;
    double y;
    double pressure;
    uint32_t timeMs;
};

class CanvasStabilizer {
public:
    explicit CanvasStabilizer(StabilizerConfig config = {}): config(config) {}

    void begin(StabilizerSample first) {
        reset();
        lastInput = first;
        lastPainted = first;
        previousPainted = first;
        lastLive = first;
        lastTimestamp = first.timeMs;
        hasInput = true;
        if (config.averaging == AveragingMethod::Arithmetic) {
            const auto count = std::clamp<size_t>(config.bufferSize, 1, 256);
            arithmeticBuffer.assign(count, first);
        } else if (config.averaging == AveragingMethod::VelocityGaussian) {
            gaussianBuffer.push_back({first, 0});
        }
    }

    StabilizerSample process(StabilizerSample input) {
        lastInput = input;
        previousPainted = lastPainted;
        StabilizerSample filtered = input;
        if (config.preprocessor == StrokePreprocessor::Deadzone) {
            const double dx = input.x - lastPainted.x;
            const double dy = input.y - lastPainted.y;
            const double distance = std::hypot(dx, dy);
            if (distance <= std::max(0.0, config.deadzoneRadius)) {
                lastLive = input;
                return lastPainted;
            }
            if (config.cuspDetection) {
                const double oldX = lastLive.x - lastPainted.x;
                const double oldY = lastLive.y - lastPainted.y;
                if (oldX * dx + oldY * dy < 0) {
                    lastLive = input;
                    resetAveraging(input);
                    lastPainted = input;
                    return input;
                }
            }
            const double ratio = config.deadzoneRadius / distance;
            filtered.x = input.x - ratio * dx;
            filtered.y = input.y - ratio * dy;
            lastLive = input;
        } else if (config.preprocessor == StrokePreprocessor::Inertia) {
            const double safeMass = std::max(config.mass, 0.01);
            speedX = (1.0 - std::clamp(config.drag, 0.0, 1.0)) * speedX + (input.x - lastPainted.x) / safeMass;
            speedY = (1.0 - std::clamp(config.drag, 0.0, 1.0)) * speedY + (input.y - lastPainted.y) / safeMass;
            filtered.x = lastPainted.x + speedX;
            filtered.y = lastPainted.y + speedY;
        }

        if (config.averaging == AveragingMethod::Arithmetic) {
            const auto count = std::clamp<size_t>(config.bufferSize, 1, 256);
            arithmeticBuffer.push_front(filtered);
            while (arithmeticBuffer.size() > count) arithmeticBuffer.pop_back();
            filtered = {};
            for (const auto& sample: arithmeticBuffer) {
                filtered.x += sample.x;
                filtered.y += sample.y;
                filtered.pressure += sample.pressure;
            }
            const double divisor = static_cast<double>(arithmeticBuffer.size());
            filtered.x /= divisor;
            filtered.y /= divisor;
            filtered.pressure /= divisor;
            filtered.timeMs = input.timeMs;
        } else if (config.averaging == AveragingMethod::VelocityGaussian) {
            filtered = gaussianAverage(filtered);
        }
        lastPainted = filtered;
        return filtered;
    }

    std::vector<StabilizerSample> finalize() const {
        if (!config.finalizeStroke || !hasInput ||
            std::hypot(lastInput.x - lastPainted.x, lastInput.y - lastPainted.y) < 0.5) {
            return {};
        }
        const auto& a = previousPainted;
        const auto& b = lastPainted;
        const auto& c = lastInput;
        const double m0x = b.x - a.x;
        const double m0y = b.y - a.y;
        const double m1x = c.x - b.x;
        const double m1y = c.y - b.y;
        std::vector<StabilizerSample> tail;
        tail.reserve(4);
        for (int i = 1; i <= 4; ++i) {
            const double t = i / 4.0;
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double h00 = 2 * t3 - 3 * t2 + 1;
            const double h10 = t3 - 2 * t2 + t;
            const double h01 = -2 * t3 + 3 * t2;
            const double h11 = t3 - t2;
            tail.push_back({h00*b.x + h10*m0x + h01*c.x + h11*m1x,
                            h00*b.y + h10*m0y + h01*c.y + h11*m1y,
                            b.pressure + (c.pressure - b.pressure) * t, c.timeMs});
        }
        return tail;
    }

    void reset() {
        arithmeticBuffer.clear();
        gaussianBuffer.clear();
        speedX = 0;
        speedY = 0;
        lastInput = {};
        lastPainted = {};
        previousPainted = {};
        lastLive = {};
        hasInput = false;
    }

private:
    struct VelocitySample {
        StabilizerSample point;
        double velocity;
    };

    void resetAveraging(StabilizerSample sample) {
        arithmeticBuffer.clear();
        gaussianBuffer.clear();
        if (config.averaging == AveragingMethod::Arithmetic) {
            arithmeticBuffer.assign(std::clamp<size_t>(config.bufferSize, 1, 256), sample);
        } else if (config.averaging == AveragingMethod::VelocityGaussian) {
            gaussianBuffer.push_back({sample, 0});
        }
    }

    StabilizerSample gaussianAverage(StabilizerSample input) {
        double elapsed = static_cast<double>(input.timeMs - lastTimestamp);
        if (gaussianBuffer.empty()) {
            gaussianBuffer.push_front({input, 0});
        } else {
            if (elapsed == 0) elapsed = 1;
            const auto& previous = gaussianBuffer.front().point;
            const double velocity = std::hypot(input.x - previous.x, input.y - previous.y) / elapsed;
            gaussianBuffer.push_front({input, velocity});
        }
        lastTimestamp = input.timeMs;
        const double twoSigmaSquared = 2.0 * std::max(config.sigma, 0.01) * std::max(config.sigma, 0.01);
        StabilizerSample sum{0, 0, 0, input.timeMs};
        double velocitySum = 0;
        double weightSum = 0;
        for (auto it = gaussianBuffer.begin(); it != gaussianBuffer.end(); ++it) {
            const double weight = std::exp(-(velocitySum * velocitySum) / twoSigmaSquared);
            if (weight < 0.01) {
                gaussianBuffer.erase(it, gaussianBuffer.end());
                break;
            }
            sum.x += weight * it->point.x;
            sum.y += weight * it->point.y;
            sum.pressure += weight * it->point.pressure;
            weightSum += weight;
            velocitySum += it->velocity;
        }
        if (weightSum > 0) {
            sum.x /= weightSum;
            sum.y /= weightSum;
            sum.pressure /= weightSum;
        } else {
            sum = input;
        }
        return sum;
    }

    StabilizerConfig config;
    std::deque<StabilizerSample> arithmeticBuffer;
    std::deque<VelocitySample> gaussianBuffer;
    StabilizerSample lastInput{};
    StabilizerSample lastPainted{};
    StabilizerSample previousPainted{};
    StabilizerSample lastLive{};
    uint32_t lastTimestamp = 0;
    double speedX = 0;
    double speedY = 0;
    bool hasInput = false;
};

}  // namespace prototype
