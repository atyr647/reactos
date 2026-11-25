#include <windows.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdarg.h>
static void L(const char *fmt, ...) { char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); strcat(b, "\n"); OutputDebugStringA(b); }
static LONG WINAPI Uef(PEXCEPTION_POINTERS p) { L("GATHER: CRASH %08lx at %p addr %llx", p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress, (unsigned long long)p->ExceptionRecord->ExceptionInformation[1]); ExitProcess(1); return 0; }
__attribute__((target("avx2"))) int main(void)
{
    static int tbl[64]; int i;
    __m256i idx, mask, src, r; int out[8];
    SetUnhandledExceptionFilter(Uef);
    for (i = 0; i < 64; i++) tbl[i] = i * 3;
    /* all lanes enabled, valid indices */
    idx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    r = _mm256_i32gather_epi32(tbl, idx, 4);
    _mm256_storeu_si256((__m256i *)out, r);
    L("GATHER: plain: %d %d %d %d %d %d %d %d", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
    /* masked-off lanes carry garbage indices that would fault if accessed */
    idx = _mm256_setr_epi32(1, (int)0xAE177800, 3, (int)0xF5E9D400, 5, 0x7fffffff, 7, (int)0x80000000);
    mask = _mm256_setr_epi32(-1, 0, -1, 0, -1, 0, -1, 0);
    src = _mm256_set1_epi32(-7);
    r = _mm256_mask_i32gather_epi32(src, tbl, idx, mask, 4);
    _mm256_storeu_si256((__m256i *)out, r);
    L("GATHER: masked: %d %d %d %d %d %d %d %d (expect 3 -7 9 -7 15 -7 21 -7)", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
    L("GATHER: PASS");
    return 0;
}
