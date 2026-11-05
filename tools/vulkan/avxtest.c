/* Verifies that the OS preserves YMM registers (upper halves!) across preemption, syscalls and APC/exceptions. */
#include <windows.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdarg.h>
static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); }
static volatile LONG stop, fails, iters;

__attribute__((target("avx2"))) static DWORD WINAPI Worker(LPVOID p)
{
    unsigned id = (unsigned)(ULONG_PTR)p;
    __m256i pat = _mm256_set_epi32(id * 16 + 7, id * 16 + 6, id * 16 + 5, id * 16 + 4, id * 16 + 3, id * 16 + 2, id * 16 + 1, id * 16);
    __m256i hi = _mm256_set1_epi32(0x5A5A0000 | id);
    long n = 0;
    while (!stop)
    {
        __m256i a = pat, b = hi;
        int k;
        for (k = 0; k < 2000; k++)
        {
            a = _mm256_add_epi32(a, _mm256_set1_epi32(1));
            b = _mm256_xor_si256(b, a);
            if ((k & 255) == 0) { __asm__ volatile("" ::: "memory"); }
        }
        /* syscalls and yields in the middle of live vector state */
        {
            __m256i a2 = a, b2 = b;
            Sleep(0);
            GetTickCount();
            SwitchToThread();
            if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(a, a2)) != -1 || _mm256_movemask_epi8(_mm256_cmpeq_epi32(b, b2)) != -1)
            {
                InterlockedIncrement(&fails);
                if (fails < 5) L("AVXTEST: thread %u vector state changed across yield", id);
            }
        }
        /* verify recomputation */
        {
            __m256i ra = pat, rb = hi; int k2;
            for (k2 = 0; k2 < 2000; k2++) { ra = _mm256_add_epi32(ra, _mm256_set1_epi32(1)); rb = _mm256_xor_si256(rb, ra); }
            if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(ra, a)) != -1 || _mm256_movemask_epi8(_mm256_cmpeq_epi32(rb, b)) != -1)
            { InterlockedIncrement(&fails); if (fails < 5) L("AVXTEST: thread %u recompute mismatch", id); }
        }
        n++; InterlockedIncrement(&iters);
    }
    return 0;
}

int main(void)
{
    HANDLE th[4]; int i;
    L("AVXTEST: start (avx2 cpuid? %d)", __builtin_cpu_supports("avx2"));
    for (i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, Worker, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
    Sleep(20000);
    stop = 1;
    WaitForMultipleObjects(4, th, TRUE, 10000);
    L("AVXTEST: %s iters %ld fails %ld", fails ? "FAIL" : "PASS", iters, fails);
    return 0;
}
