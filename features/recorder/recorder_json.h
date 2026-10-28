// The recorder's JSON lines (recorder.cpp): bounded serialization without a CRT, no game
// calls. tests/unit/test_recorder.cpp reads its output back with the replay's parser.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace fdcore::recorder {

inline bool Finite(float f) { uint32_t bits; memcpy(&bits, &f, 4); return (bits & 0x7f800000u) != 0x7f800000u; }

// Nine significant decimal digits preserve binary32 values, including very small/large
// values; never emit NaN.
struct Json {
    char text[2048];
    unsigned size;
    bool ok;
    void Begin() { size = 0; ok = true; Raw("{"); }
    void Raw(const char* s) {
        unsigned n = static_cast<unsigned>(lstrlenA(s));
        if (size + n >= sizeof(text)) { ok = false; return; }
        memcpy(text + size, s, n); size += n; text[size] = 0;
    }
    void String(const char* s) {
        Raw("\"");
        for (; *s; ++s) {
            unsigned char c = static_cast<unsigned char>(*s);
            char b[8] = {};
            if (c == '"' || c == '\\') { b[0] = '\\'; b[1] = static_cast<char>(c); }
            else if (c < 32 || c >= 127) wsprintfA(b, "\\u%04x", static_cast<unsigned>(c));
            else b[0] = static_cast<char>(c);
            Raw(b);
        }
        Raw("\"");
    }
    void Key(const char* key) { if (size > 1) Raw(","); String(key); Raw(":"); }
    void U32(uint32_t n) { char b[16]; wsprintfA(b, "%lu", static_cast<unsigned long>(n)); Raw(b); }
    void Int(int n) { char b[16]; wsprintfA(b, "%d", n); Raw(b); }
    void Number(float f) {
        if (!Finite(f)) { ok = false; return; }
        if (f == 0) {
            uint32_t bits; memcpy(&bits, &f, 4);
            Raw(bits >> 31 ? "-0.0" : "0"); return;
        }
        double d = f; bool negative = d < 0;
        if (negative) d = -d;
        int exponent = 0;
        while (d >= 10) { d /= 10; ++exponent; }
        while (d < 1) { d *= 10; --exponent; }
        uint32_t digits = static_cast<uint32_t>(d * 100000000.0 + 0.5);
        if (digits >= 1000000000u) { digits /= 10; ++exponent; }
        char b[48];
        wsprintfA(b, "%s%lu.%08lue%d", negative ? "-" : "", static_cast<unsigned long>(digits / 100000000u),
                  static_cast<unsigned long>(digits % 100000000u), exponent);
        Raw(b);
    }
    void Vector(const float* v) { Raw("["); Number(v[0]); Raw(","); Number(v[1]); Raw(","); Number(v[2]); Raw("]"); }
    void Text(const char* k, const char* v) { Key(k); String(v); }
    void UInt(const char* k, uint32_t v) { Key(k); U32(v); }
    void Vec(const char* k, const float* v) { Key(k); Vector(v); }
};

} // namespace fdcore::recorder
