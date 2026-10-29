// Unit tests (D47): core code compiled into a host program, no game. Each test is a
// function listed in main.cpp; CHECK records a failure and carries on.
#pragma once

#include <stdio.h>

namespace unit {

extern int g_checks, g_failures;
extern const char* g_test;

inline void Check(bool ok, const char* what, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAIL %s: %s (%s:%d)\n", g_test, what, file, line);
}

// Log lines the code under test wrote since the last ClearLog (stubs.cpp).
bool LogContains(const char* text);
int LogCount(const char* text);
void ClearLog();
// What console::GameDir returns (with a trailing backslash).
void SetGameDir(const char* dir);

} // namespace unit

#define CHECK(cond) ::unit::Check((cond), #cond, __FILE__, __LINE__)
