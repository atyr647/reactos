/* Checks that pointer-sized values survive user32/gdi32/kernel32 round trips (64-bit truncation hunting). */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); }
static int bad;
#define CHK(name, got, want) do { unsigned long long g_ = (unsigned long long)(got), w_ = (unsigned long long)(want); if (g_ != w_) { bad++; L("PTRTEST: FAIL %s got %llx want %llx", name, g_, w_); } else L("PTRTEST: ok   %s", name); } while (0)
static LPARAM lp_seen; static LRESULT ret_val = 0x1B605D400ull;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CREATE) lp_seen = (LPARAM)((CREATESTRUCTA *)l)->lpCreateParams;
    if (m == WM_USER + 1) { lp_seen = l; return ret_val; }
    return DefWindowProcA(h, m, w, l);
}
int main(void)
{
    const ULONG_PTR V = 0x00000001B605D400ull, V2 = 0xFFFFF8A0B605D400ull & 0x00007FFFFFFFFFFFull;
    WNDCLASSA wc = { 0 }; HWND hwnd; LRESULT r;
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "ptrt"; wc.cbClsExtra = 16; wc.cbWndExtra = 32;
    RegisterClassA(&wc);
    hwnd = CreateWindowA("ptrt", "t", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, wc.hInstance, (LPVOID)V);
    CHK("CreateWindow lpCreateParams", lp_seen, V);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, V); CHK("GWLP_USERDATA", GetWindowLongPtrA(hwnd, GWLP_USERDATA), V);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, V2); CHK("GWLP_USERDATA W", GetWindowLongPtrW(hwnd, GWLP_USERDATA), V2);
    SetWindowLongPtrA(hwnd, 0, V); CHK("WndExtra[0]", GetWindowLongPtrA(hwnd, 0), V);
    SetWindowLongPtrA(hwnd, 8, V2); CHK("WndExtra[8]", GetWindowLongPtrA(hwnd, 8), V2);
    SetClassLongPtrA(hwnd, 0, V); CHK("ClsExtra[0]", GetClassLongPtrA(hwnd, 0), V);
    SetPropA(hwnd, "p", (HANDLE)V); CHK("Prop", GetPropA(hwnd, "p"), V);
    r = SendMessageA(hwnd, WM_USER + 1, V, V); CHK("SendMessage wparam/lparam", lp_seen, V); CHK("SendMessage result", r, ret_val);
    CHK("GWLP_HINSTANCE", GetWindowLongPtrA(hwnd, GWLP_HINSTANCE), (ULONG_PTR)wc.hInstance);
    CHK("GWLP_WNDPROC", GetWindowLongPtrA(hwnd, GWLP_WNDPROC) != 0, 1);
    {   /* generic heap/virtual memory in the 2-4GB range */
        void *p = VirtualAlloc((void *)0xB6000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        L("PTRTEST: VirtualAlloc fixed 0xB6000000 -> %p", p);
        if (p) { SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)p); CHK("userdata fixed block", GetWindowLongPtrA(hwnd, GWLP_USERDATA), (ULONG_PTR)p); }
    }
    L("PTRTEST: %s (%d failures)", bad ? "FAIL" : "PASS", bad);
    return 0;
}
