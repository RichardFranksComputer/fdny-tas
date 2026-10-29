// Tests: tests/unit/test_crashlog.cpp
// Logs fatal-looking exceptions (first chance) to FDNYCore.log with the
// faulting module+offset and the code addresses found on the stack, so a
// crash can be traced to a function in Ghidra without a debugger attached.
#include "crashlog.h"

#include <windows.h>
#include <stdint.h>

#include "log.h"

namespace fdcore::crashlog {
namespace {

volatile long g_reports = 0;

void Describe(uintptr_t a, char* out) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &m) &&
        m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = name;
        for (const char* p = name; *p; ++p)
            if (*p == '\\') base = p + 1;
        wsprintfA(out, "%08lx (%s+%lx)", static_cast<unsigned long>(a), base,
                  static_cast<unsigned long>(a - reinterpret_cast<uintptr_t>(m)));
    } else {
        wsprintfA(out, "%08lx (no module)", static_cast<unsigned long>(a));
    }
}

bool IsCode(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_reports) > 8) return EXCEPTION_CONTINUE_SEARCH;  // don't flood
    char where[MAX_PATH + 64];
    Describe(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress), where);
    const CONTEXT* c = ep->ContextRecord;
    log::Warn("crashlog: exception %08lx at %s; data %08lx", code, where,
              ep->ExceptionRecord->NumberParameters > 1
                  ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionInformation[1])
                  : 0ul);
    log::Warn("crashlog: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Code addresses on the stack = likely return addresses (a cheap callstack).
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(c->Esp);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 12; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = sp[i];
        if (v > 0x10000 && IsCode(v)) {
            Describe(v, where);
            log::Warn("crashlog:   [esp+%03x] %s", i * 4, where);
            ++shown;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;  // the game's own handling proceeds unchanged
}

} // namespace

void Install() { AddVectoredExceptionHandler(1, Handler); }

} // namespace fdcore::crashlog
