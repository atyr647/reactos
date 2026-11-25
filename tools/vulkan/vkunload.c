#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); }
static LONG WINAPI Veh(PEXCEPTION_POINTERS p)
{
    HMODULE m = NULL; char name[260] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, p->ExceptionRecord->ExceptionAddress, &m);
    if (m) GetModuleFileNameA(m, name, sizeof name);
    L("VKUNL: EXCEPTION %08lx at %p (%s +%llx) info0 %llx info1 %llx", p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress,
      name, (unsigned long long)((char *)p->ExceptionRecord->ExceptionAddress - (char *)m), (unsigned long long)p->ExceptionRecord->ExceptionInformation[0],
      (unsigned long long)p->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_EXECUTE_HANDLER;
}
int main(int argc, char **argv)
{
    HMODULE m;
    L("VKUNL: start");
    SetUnhandledExceptionFilter(Veh);
    L("VKUNL: filter set");
    m = LoadLibraryA(argc > 1 ? argv[1] : "C:\\vulkan_lvp.dll");
    L("VKUNL: loaded %p err %lu", m, GetLastError());
    if (!m) return 1;
    L("VKUNL: calling FreeLibrary");
    L("VKUNL: FreeLibrary -> %d", FreeLibrary(m));
    m = LoadLibraryA(argc > 1 ? argv[1] : "C:\\vulkan_lvp.dll");
    L("VKUNL: reloaded %p err %lu", m, GetLastError());
    L("VKUNL: done");
    return 0;
}
