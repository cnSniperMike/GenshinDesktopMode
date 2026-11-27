#include "map_flick.h"
#include <cassert>

static void checkRelease(int fps, int height, double direction, bool finalMovement) {
    DesktopMapFlick flick;
    const double dt = 1. / fps, dx = direction * height * 1.5 * dt, dy = -dx / 2;
    flick.begin(0);
    for (int i=1; i<=fps/2; ++i) flick.dragFrame(dx, dy, height, i * dt);
    const double releaseTime = .5 + dt;
    if (finalMovement) flick.dragFrame(dx, dy, height, releaseTime);
    assert(flick.release(releaseTime));
    double distance = 0, previous = std::abs(dx);
    for (int frame=0; frame<fps && flick.active(); ++frame) {
        auto delta = flick.advance(height, releaseTime + frame * dt);
        if (finalMovement && frame == 0) {
            assert(delta.x == 0 && delta.y == 0);
            continue;
        }
        assert(delta.x * direction > 0 && std::abs(delta.y + delta.x/2) < .0001);
        assert(std::abs(delta.x) <= previous + .0001);
        if (frame == (finalMovement ? 1 : 0)) assert(std::abs(delta.x) > std::abs(dx) * .85);
        previous = std::abs(delta.x);
        distance += delta.x / height;
    }
    assert(!flick.active());
    const double expected = direction * 1.5 * .115 * (1. - std::exp(-.42 / .115));
    assert(std::abs(expected - distance) < .00001);
}

int main() {
    for (int fps : {30, 60, 120, 240}) {
        checkRelease(fps, 1080, 1, false);
        checkRelease(fps, 2160, -1, true);
    }
    DesktopMapFlick f;
    f.begin(0); assert(!f.release(.01));  // Click without a drag.
    f.begin(1); f.dragFrame(8,0,1000,1.04); assert(!f.release(1.05));
    f.begin(2); f.dragFrame(48,-20,1000,2.02); assert(!f.release(2.2));
    f.begin(3); f.dragFrame(70,0,1000,3.03); assert(f.release(3.04));
    assert(f.advance(1000,3.8).x==0 && !f.active());
    f.begin(4); f.dragFrame(70,0,1000,4.03); assert(f.release(4.04));
    f.cancel(); assert(f.advance(1000,4.05).x==0);
    f.begin(5); f.dragFrame(18,0,1000,5.02);
    f.dragFrame(0,0,1000,5.065); assert(!f.release(5.07));
    f.begin(6); f.dragFrame(40,0,1000,6.02); f.dragFrame(0,0,1000,6.03);
    assert(f.release(6.04));
    // A still frame before release is already presented and must not be backfilled.
    auto d = f.advance(1000,6.04); assert(d.x>10 && d.x<15);
    f.begin(7); assert(!f.active() && !f.release(7.01));
}
