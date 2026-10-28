#include "unit.h"
#include "../../core/spectator_math.h"
#include "../../core/spectator.h"
namespace {
bool Near(float a, float b) {
    return a - b < 0.001f && b - a < 0.001f;
}
} // namespace
void TestSpectatorCamera() {
    fdcore::spectator::input::Key key = {};
    key.Reset(false);
    // The first tap can finish between viewer frames: deliver it once anyway.
    key.Message(false);
    key.Release();
    CHECK(key.Poll(false, true));
    CHECK(!key.Poll(false, true));
    // A message and polling the same held key must not fire twice.
    key.Message(false);
    CHECK(key.Poll(true, true));
    key.Message(true);
    CHECK(!key.Poll(true, true));
    CHECK(!key.Poll(false, true));
    // Polling may see a press before its queued window message arrives.
    CHECK(key.Poll(true, true));
    key.Message(false);
    CHECK(!key.Poll(true, true));
    // A release followed by another tap between polls is a new press.
    key.Release();
    key.Message(false);
    key.Release();
    CHECK(key.Poll(false, true));
    // Held keys at startup and input outside focus/the console stay ignored.
    key.Reset(true);
    key.Message(true);
    CHECK(!key.Poll(true, true));
    CHECK(!key.Poll(false, true));
    key.Message(false);
    CHECK(!key.Poll(false, false));
    CHECK(!key.Poll(false, true));
    key.Message(false);
    key.Reset(false);
    CHECK(!key.Poll(false, true));
    namespace m = fdcore::spectator::math;
    CHECK(Near(m::Wrap(-450), 270));
    CHECK(Near(m::Wrap(810), 90));
    CHECK(Near(m::Yaw(0, 1), 0));
    CHECK(Near(m::Yaw(1, 0), 90));
    CHECK(Near(m::Yaw(-1, 0), 270));
    CHECK(Near(m::Zoom(240, 1), 204));
    CHECK(Near(m::Zoom(m::Zoom(240, 1), -1), 240));
    CHECK(Near(m::Zoom(60, 20), 60));
    CHECK(Near(m::Zoom(2400, -20), 2400));
    float target[3] = {100, 10, 200}, pos[3];
    m::Orbit(target, 0, 0, 240, pos);
    CHECK(Near(pos[0], 100));
    CHECK(Near(pos[1], 55));
    CHECK(Near(pos[2], -40));
    m::Orbit(target, 90, 0, 240, pos);
    CHECK(Near(pos[0], -140));
    CHECK(Near(pos[2], 200));
    m::Orbit(target, 0, -30, 240, pos);
    CHECK(Near(pos[1], 175));
    fdcore::camera::XForm x;
    m::Build(pos, 0, -30, &x);
    for (int row = 0; row < 3; ++row)
        CHECK(Near(x.t[row], pos[row]));
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
            float dot = 0;
            for (int row = 0; row < 3; ++row)
                dot += x.m[row * 3 + a] * x.m[row * 3 + b];
            CHECK(Near(dot, a == b ? 1.0f : 0.0f));
        }
    // The free camera's start pose: yaw/pitch recovered from a view matrix's -column 2.
    {
        const float p0[3] = {1, 2, 3};
        const float yaws[] = {0, 45, 135, 200, 315}, pitches[] = {-60, -10, 0, 30, 80};
        for (float yaw : yaws)
            for (float pitch : pitches) {
                fdcore::camera::XForm v;
                m::Build(p0, yaw, pitch, &v);
                const float fwd[3] = {-v.m[2], -v.m[5], -v.m[8]};
                float y2 = -1, p2 = -1;
                m::Angles(fwd, &y2, &p2);
                CHECK(Near(y2, yaw) || Near(y2 - 360, yaw));
                CHECK(Near(p2, pitch));
            }
    }
    // The camera looks directly at the lifted torso, including nonzero yaw/pitch.
    m::Orbit(target, 135, -22, 360, pos);
    m::Build(pos, 135, -22, &x);
    float delta[3] = {target[0] - pos[0], target[1] + 45 - pos[1], target[2] - pos[2]};
    CHECK(Near(delta[0] * x.m[0] + delta[1] * x.m[3] + delta[2] * x.m[6], 0));
    CHECK(Near(delta[0] * x.m[1] + delta[1] * x.m[4] + delta[2] * x.m[7], 0));
    CHECK(Near(delta[0] * x.m[2] + delta[1] * x.m[5] + delta[2] * x.m[8], -360));
}
