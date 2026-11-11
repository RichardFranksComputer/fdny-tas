// Numbers as text, without the CRT (wsprintf has no %f): one parser and one formatter for
// every variable, console line and panel. Header-only, so the unit tests use it as is.
#pragma once
#include <windows.h>
#include <xmmintrin.h>

namespace fdcore::num {

// "-12.5", "+3", ".5": the whole string must be a number.
inline bool Parse(const char* s, float* out) {
    // Digits as one whole number over a power of ten, in doubles: "0.075" is exactly 0.075f
    // (summing tenths in floats is a few bits off).
    double sign = 1, digits = 0, over = 1;
    if (*s == '-') sign = -1, ++s;
    else if (*s == '+') ++s;
    bool any = false;
    while (*s >= '0' && *s <= '9') digits = digits * 10 + (*s++ - '0'), any = true;
    if (*s == '.') {
        ++s;
        while (*s >= '0' && *s <= '9') digits = digits * 10 + (*s++ - '0'), over *= 10, any = true;
    }
    if (!any || *s) return false;
    *out = static_cast<float>(sign * digits / over);
    return true;
}

inline long Round(float v) { return static_cast<long>(v + (v >= 0 ? 0.5f : -0.5f)); }

// v with `decimals` (0-6) digits after the point, rounded: Fixed(out, -123.44f, 1) = "-123.4".
// Returns the length. out needs 24 chars.
inline int Fixed(char* out, float v, int decimals) {
    long scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const long m = Round(v * static_cast<float>(scale)), a = m < 0 ? -m : m;
    if (decimals <= 0) return wsprintfA(out, "%ld", m);
    char frac[8];
    long f = a % scale;
    for (int i = decimals - 1; i >= 0; --i) frac[i] = static_cast<char>('0' + f % 10), f /= 10;
    frac[decimals] = 0;
    return wsprintfA(out, "%s%ld.%s", m < 0 ? "-" : "", a / scale, frac);
}

// Up to 3 decimals, trailing zeros dropped: 0.075, 1.4, 400 (a variable's range or value).
inline int Short(char* out, float v) {
    int n = Fixed(out, v, 3);
    while (n > 1 && out[n - 1] == '0') out[--n] = 0;
    if (n > 1 && out[n - 1] == '.') out[--n] = 0;
    return n;
}

inline float Sqrt(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }

} // namespace fdcore::num
