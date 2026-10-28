// Pure spectator-camera geometry, shared with the host unit tests.
#pragma once
#include "camera.h"
namespace fdcore::spectator::math {
constexpr float kDegrees = 0.017453292f;
inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
inline float Wrap(float a) {
    while (a >= 360)
        a -= 360;
    while (a < 0)
        a += 360;
    return a;
}
inline void SinCos(float degrees, float* s, float* c) {
    const float radians = degrees * kDegrees;
    __asm__("fsincos" : "=t"(*c), "=u"(*s) : "0"(radians));
}
inline float Sqrt(float v) {
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(v));
    return r;
}
inline float Atan2(float y, float x) {  // degrees, with the quadrant (x87 fpatan)
    float r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r / kDegrees;
}
inline float Yaw(float x, float z) {
    return Wrap(Atan2(x, z));
}
// Yaw and pitch of a view direction (forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)).
inline void Angles(const float* forward, float* yaw, float* pitch) {
    *yaw = Yaw(forward[0], forward[2]);
    *pitch = Atan2(forward[1], Sqrt(forward[0] * forward[0] + forward[2] * forward[2]));
}
inline void Build(const float* pos, float yaw, float pitch, camera::XForm* out) {
    float sy, cy, sp, cp;
    SinCos(yaw, &sy, &cy);
    SinCos(pitch, &sp, &cp);
    const float f[3] = {sy * cp, sp, cy * cp}, r[3] = {-cy, 0, sy};
    const float u[3] = {-r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1]};
    for (int i = 0; i < 3; ++i) {
        out->m[i * 3] = r[i];
        out->m[i * 3 + 1] = u[i];
        out->m[i * 3 + 2] = -f[i];
        out->t[i] = pos[i];
    }
}
// pitch is the camera's look pitch: negative puts the orbit camera above its
// target while looking down. Target is sampled player origin, raised to torso.
inline void Orbit(const float* target, float yaw, float pitch, float distance, float* pos) {
    float sy, cy, sp, cp;
    SinCos(yaw, &sy, &cy);
    SinCos(pitch, &sp, &cp);
    distance = Clamp(distance, 60, 2400);
    pos[0] = target[0] - sy * cp * distance;
    pos[1] = target[1] + 45 - sp * distance;
    pos[2] = target[2] - cy * cp * distance;
}
inline float Zoom(float distance, float notches) {
    notches = Clamp(notches, -20, 20);
    while (notches >= 1) {
        distance *= 0.85f;
        notches -= 1;
    }
    while (notches <= -1) {
        distance /= 0.85f;
        notches += 1;
    }
    return Clamp(distance * (1 - 0.15f * notches), 60, 2400);
}
} // namespace fdcore::spectator::math
