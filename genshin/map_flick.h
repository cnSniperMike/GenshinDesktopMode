#pragma once
#include <algorithm>
#include <cmath>

class DesktopMapFlick {
public:
    struct Delta { float x=0, y=0; };

    void cancel() { *this = {}; }
    void begin(double now) {
        cancel();
        lastDragFrame = lastMotion = now;
    }

    // Record every presented drag frame, including one that stayed still.
    // A mouse-up frame is recorded here only if it supplied real movement.
    void dragFrame(double dx, double dy, int height, double now) {
        lastDragFrame = now;
        if (dx == 0 && dy == 0) return;
        const double dt = now - lastMotion;
        lastMotion = now;
        if (height <= 0 || !std::isfinite(dx) || !std::isfinite(dy) ||
            !std::isfinite(dt) || dt <= 0 || dt > .12) {
            vx = vy = 0;
            hasVelocity = false;
            return;
        }
        const double x = dx / (height * dt), y = dy / (height * dt);
        const double cap = 3. / std::max(3., std::hypot(x, y));
        const double weight = hasVelocity ? -std::expm1(-dt / .026) : 1.;
        vx += (x * cap - vx) * weight;
        vy += (y * cap - vy) * weight;
        hasVelocity = true;
    }

    bool release(double now) {
        const double age = now - lastMotion;
        if (!hasVelocity || !std::isfinite(age) || age < 0 || age > .075) return false;
        const double idleDecay = std::exp(-std::max(0., lastDragFrame - lastMotion) / .030);
        vx *= idleDecay;
        vy *= idleDecay;
        const double speed = std::hypot(vx, vy);
        if (speed < .28) return false;
        // An unchanged release position leaves one frame of travel to draw.
        // A new release position already covers the same interval exactly.
        previousFrame = lastDragFrame;
        endTime = previousFrame + std::min(.42, decayTime * std::log(speed / .025));
        coasting = true;
        return true;
    }

    bool active() const { return coasting; }

    Delta advance(int height, double now) {
        if (!coasting) return {};
        const double dt = now - previousFrame;
        if (height <= 0 || !std::isfinite(dt) || dt < 0 || dt > .12) {
            cancel();
            return {};
        }
        const double elapsed = std::min(now, endTime) - previousFrame;
        const double fraction = -std::expm1(-elapsed / decayTime);
        const double distance = height * decayTime * fraction;
        Delta result{float(vx * distance), float(vy * distance)};
        vx *= 1. - fraction;
        vy *= 1. - fraction;
        previousFrame = now;
        if (now >= endTime) cancel();
        return result;
    }

private:
    static constexpr double decayTime = .115;
    double vx = 0, vy = 0, lastMotion = 0, lastDragFrame = 0;
    double previousFrame = 0, endTime = 0;
    bool hasVelocity = false, coasting = false;
};
