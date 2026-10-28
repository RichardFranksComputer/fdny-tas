// juggle_math: the arrow's direction for every view yaw (replaces fire_juggle's eight in-game
// yaws; the game test keeps two, proving the arrow is view-relative in the game).
#include <math.h>

#include "../../features/juggle/juggle_math.h"
#include "unit.h"

namespace j = fdcore::juggle;

void TestJuggleDirection() {
    // fire_juggle's setup: the landing point 60 u to -X of the player. At yaw 0 the view
    // looks along +Z, so -X is to the right; turning the view by 45 degrees steps clockwise.
    const int want[8] = {2, 3, 4, 5, 6, 7, 0, 1};  // yaw 0, 45, ..., 315
    for (int i = 0; i < 8; ++i) {
        const float rad = static_cast<float>(i * 45) * 0.017453292f;
        CHECK(j::Direction(-60, 0, 60, sinf(rad), cosf(rad)) == want[i]);
    }
    // Within kNear: "o", whatever the view.
    CHECK(j::Direction(-10, 5, 11.2f, 0, 1) == j::kDirNear);
    CHECK(j::Direction(-16, 0, 16, 0, 1) == 2);  // 16 u is no longer near
    // Sector edges: 22.5 degrees off an axis still belongs to that axis' sector.
    CHECK(j::Sector(1, 0) == 0 && j::Sector(-1, 0) == 4 && j::Sector(0, 1) == 2 && j::Sector(0, -1) == 6);
    CHECK(j::Sector(1, j::kTan22) == 0 && j::Sector(1, j::kTan22 * 1.01f) == 1);
    CHECK(j::Sector(1, 1) == 1 && j::Sector(-1, 1) == 3 && j::Sector(-1, -1) == 5 && j::Sector(1, -1) == 7);
}
