/* Minimal user-mode debugger for ReactOS bring-up: runs a program under DEBUG_PROCESS and logs
 * (via OutputDebugString = serial) every exception with address/module/registers and a stack scan. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tlhelp32.h>

static void L(const char *fmt, ...)
{
    char b[600]; va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    strcat(b, "\n"); OutputDebugStringA(b);
}

typedef struct { ULONG64 base; char name[200]; } MOD;
static MOD mods[256]; static int nmods;

static MOD *find(ULONG64 a)
{
    int i, best = -1;
    for (i = 0; i < nmods; i++)
        if (mods[i].base <= a && a - mods[i].base < 0x10000000 && (best < 0 || mods[i].base > mods[best].base)) best = i;
    return best < 0 ? NULL : &mods[best];
}
static const char *where(ULONG64 a, char *buf, size_t n)
{
    MOD *m = find(a);
    if (m) snprintf(buf, n, "%s+%llx", m->name, (unsigned long long)(a - m->base)); else snprintf(buf, n, "?");
    return buf;
}

static void readname(HANDLE proc, LPVOID imgname, WORD unicode, char *out, size_t n)
{
    LPVOID p = NULL; SIZE_T r; char tmp[400] = { 0 };
    out[0] = 0;
    if (!imgname || !ReadProcessMemory(proc, imgname, &p, sizeof p, &r) || !p) return;
    if (!ReadProcessMemory(proc, p, tmp, sizeof tmp - 2, &r)) { ReadProcessMemory(proc, p, tmp, 64, &r); }
    if (unicode) { WideCharToMultiByte(CP_ACP, 0, (WCHAR *)tmp, -1, out, (int)n, 0, 0); }
    else strncpy(out, tmp, n - 1);
    { char *s = strrchr(out, '\\'); if (s) memmove(out, s + 1, strlen(s)); }
}

int main(int argc, char **argv)
{
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    char cmd[2000] = ""; int i, exceptions = 0; DEBUG_EVENT ev;
    if (argc > 3 && !strcmp(argv[1], "-a"))
    {
        /* -a <delay-ms> <cmdline...>: start normally, attach after the delay (avoids debug-launch side effects) */
        for (i = 3; i < argc; i++) { if (i > 3) strcat(cmd, " "); strcat(cmd, argv[i]); }
        L("ROSDBG: launching %s (attach after %s ms)", cmd, argv[2]);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { L("ROSDBG: CreateProcess failed %lu", GetLastError()); return 1; }
        Sleep(atoi(argv[2]));
        L("ROSDBG: attaching to pid %lu", pi.dwProcessId);
        if (!DebugActiveProcess(pi.dwProcessId)) { L("ROSDBG: DebugActiveProcess failed %lu", GetLastError()); return 1; }
        DebugSetProcessKillOnExit(FALSE);
    }
    else if (argc > 2 && !strcmp(argv[1], "-w"))
    {
        /* wait for a process by image name, then attach */
        DWORD pid = 0;
        L("ROSDBG: waiting for %s", argv[2]);
        while (!pid)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
            if (snap != INVALID_HANDLE_VALUE)
            {
                if (Process32First(snap, &pe)) do { if (!_stricmp(pe.szExeFile, argv[2])) pid = pe.th32ProcessID; } while (!pid && Process32Next(snap, &pe));
                CloseHandle(snap);
            }
            if (!pid) Sleep(500);
        }
        Sleep(argc > 3 ? atoi(argv[3]) : 0);
        L("ROSDBG: attaching to pid %lu", pid);
        if (!DebugActiveProcess(pid)) { L("ROSDBG: DebugActiveProcess failed %lu", GetLastError()); return 1; }
        pi.hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        DebugSetProcessKillOnExit(FALSE);
    }
    else
    {
    for (i = 1; i < argc; i++) { if (i > 1) strcat(cmd, " "); strcat(cmd, argv[i]); }
    L("ROSDBG: launching %s", cmd);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) { L("ROSDBG: CreateProcess failed %lu", GetLastError()); return 1; }
    }
    for (;;)
    {
        DWORD cont = DBG_CONTINUE;
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            mods[nmods].base = (ULONG64)ev.u.CreateProcessInfo.lpBaseOfImage; strcpy(mods[nmods++].name, "exe");
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (nmods < 256) {
                mods[nmods].base = (ULONG64)ev.u.LoadDll.lpBaseOfDll;
                readname(pi.hProcess, ev.u.LoadDll.lpImageName, ev.u.LoadDll.fUnicode, mods[nmods].name, sizeof mods[nmods].name);
                if (!mods[nmods].name[0]) strcpy(mods[nmods].name, "dll");
                nmods++;
            }
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT:
        {
            EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord; char w[300]; CONTEXT c; HANDLE th;
            cont = DBG_EXCEPTION_NOT_HANDLED;
            if (er->ExceptionCode == 0x80000003 || er->ExceptionCode == 0x80000004 || er->ExceptionCode == 0x40010006 || er->ExceptionCode == 0x4001000A || er->ExceptionCode == 0x406D1388) { cont = DBG_CONTINUE; break; }
            exceptions++;
            L("ROSDBG: EXCEPTION %08lx %s at %p (%s) info %llx %llx tid %lu",
              er->ExceptionCode, ev.u.Exception.dwFirstChance ? "first" : "second", er->ExceptionAddress,
              where((ULONG64)er->ExceptionAddress, w, sizeof w),
              (unsigned long long)er->ExceptionInformation[0], (unsigned long long)er->ExceptionInformation[1], ev.dwThreadId);
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ev.dwThreadId);
            if (th && ev.u.Exception.dwFirstChance && er->ExceptionCode == 0xC0000005 && exceptions < 6)
            {
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &c))
                {
                    ULONG64 stk[96]; SIZE_T r; int k, shown = 0; BYTE code[16] = { 0 };
                    L("ROSDBG:  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx", c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi);
                    L("ROSDBG:  r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx rbp %llx rsp %llx", c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15, c.Rbp, c.Rsp);
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)c.Rip, code, 16, &r))
                        L("ROSDBG:  code @rip: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11]);
                    if (ReadProcessMemory(pi.hProcess, (LPCVOID)c.Rsp, stk, sizeof stk, &r))
                        for (k = 0; k < (int)(r / 8) && shown < 14; k++)
                            if (find(stk[k])) { L("ROSDBG:  stack[%d] %llx = %s", k, stk[k], where(stk[k], w, sizeof w)); shown++; }
                }
            }
            if (th) CloseHandle(th);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            L("ROSDBG: process exited code %08lx after %d exceptions", ev.u.ExitProcess.dwExitCode, exceptions);
            return 0;
        case OUTPUT_DEBUG_STRING_EVENT:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    return 0;
}
