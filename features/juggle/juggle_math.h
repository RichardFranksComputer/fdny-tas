// The fire-juggling arrow's direction (juggle.cpp): no game calls, unit-tested in
// tests/unit/test_juggle.cpp. View yaw (docs/evidence/fire_juggle_overlay): forward =
// (sin yaw, cos yaw) in world (x, z), strafe right = (-cos yaw, sin yaw).
#pragma once

namespace fdcore::juggle {

constexpr float kNear = 16.0f;        // closer than this: "o"
constexpr float kTan22 = 0.41421356f; // tan(22.5 deg): 8 sectors of 45 deg

// 8 directions clockwise from straight ahead, then "o" (near).
constexpr int kDirNear = 8;

// Sector of (forward f, right r): 0 = ahead, clockwise in 45-degree steps.
inline int Sector(float f, float r) {
    const float af = f < 0 ? -f : f, ar = r < 0 ? -r : r;
    if (ar <= kTan22 * af) return f >= 0 ? 0 : 4;
    if (af <= kTan22 * ar) return r >= 0 ? 2 : 6;
    if (f >= 0) return r >= 0 ? 1 : 7;
    return r >= 0 ? 3 : 5;
}

// The arrow for a landing point (dx, dz) away from the player (horizontal distance dist),
// seen with the view's sin / cos yaw: kDirNear when within kNear, else Sector.
inline int Direction(float dx, float dz, float dist, float sinYaw, float cosYaw) {
    if (dist < kNear) return kDirNear;
    const float f = dx * sinYaw + dz * cosYaw;    // along forward (sin, cos)
    const float r = -dx * cosYaw + dz * sinYaw;   // along strafe right (-cos, sin)
    return Sector(f, r);
}

} // namespace fdcore::juggle
