/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Minimal PathCch* path helpers (api-ms-win-core-path-l1-1-0)
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

#define PATHCCH_MAX_CCH 0x8000

static BOOL PcIsSep(WCHAR c) { return c == L'\\' || c == L'/'; }

/* Collapses "." and ".." segments and duplicate separators; converts '/' to '\'. In place. */
static VOID
PcCanonicalize(PWSTR Path)
{
    PWSTR Src = Path, Dst = Path, Root;
    SIZE_T RootLen = 0;

    if (PcIsSep(Src[0]) && PcIsSep(Src[1]))
    {
        /* UNC or \\?\ : keep the prefix up to the first segment as is */
        RootLen = 2;
        if (Src[2] == L'?' && PcIsSep(Src[3])) RootLen = 4;
    }
    else if (((Src[0] >= L'A' && Src[0] <= L'Z') || (Src[0] >= L'a' && Src[0] <= L'z')) && Src[1] == L':')
    {
        RootLen = PcIsSep(Src[2]) ? 3 : 2;
    }
    else if (PcIsSep(Src[0]))
    {
        RootLen = 1;
    }
    Root = Path + RootLen;
    for (; Src < Root; Src++, Dst++) *Dst = (*Src == L'/') ? L'\\' : *Src;

    while (*Src)
    {
        PWSTR Seg = Src;
        SIZE_T Len;
        BOOL HadSep;

        while (*Src && !PcIsSep(*Src)) Src++;
        Len = Src - Seg;
        HadSep = PcIsSep(*Src);
        if (HadSep) Src++;

        if (Len == 0 || (Len == 1 && Seg[0] == L'.'))
            continue;
        if (Len == 2 && Seg[0] == L'.' && Seg[1] == L'.')
        {
            if (Dst > Root)
            {
                Dst--;                                  /* drop the trailing separator */
                while (Dst > Root && Dst[-1] != L'\\') Dst--;
            }
            continue;
        }
        memmove(Dst, Seg, Len * sizeof(WCHAR));
        Dst += Len;
        if (HadSep) *Dst++ = L'\\';
    }
    *Dst = UNICODE_NULL;
}

HRESULT
WINAPI
PathCchCombineEx(PWSTR Out, SIZE_T OutSize, PCWSTR Path1, PCWSTR Path2, ULONG Flags)
{
    SIZE_T L1, L2;
    BOOL Abs2;

    UNREFERENCED_PARAMETER(Flags);

    if (!Out || !OutSize || OutSize > PATHCCH_MAX_CCH) return E_INVALIDARG;
    if (!Path1 && !Path2) { Out[0] = 0; return E_INVALIDARG; }

    L1 = Path1 ? wcslen(Path1) : 0;
    L2 = Path2 ? wcslen(Path2) : 0;
    Abs2 = L2 && (PcIsSep(Path2[0]) || (L2 > 1 && Path2[1] == L':'));

    if (!L1 || Abs2)
    {
        if (L2 >= OutSize) { Out[0] = 0; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
        if (L2) memcpy(Out, Path2, (L2 + 1) * sizeof(WCHAR)); else Out[0] = 0;
    }
    else
    {
        BOOL Sep = L1 && !PcIsSep(Path1[L1 - 1]) && L2;
        if (L1 + (Sep ? 1 : 0) + L2 >= OutSize) { Out[0] = 0; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
        memcpy(Out, Path1, L1 * sizeof(WCHAR));
        if (Sep) Out[L1++] = L'\\';
        if (L2) memcpy(Out + L1, Path2, (L2 + 1) * sizeof(WCHAR)); else Out[L1] = 0;
    }
    PcCanonicalize(Out);
    return S_OK;
}

HRESULT
WINAPI
PathCchCombine(PWSTR Out, SIZE_T OutSize, PCWSTR Path1, PCWSTR Path2)
{
    return PathCchCombineEx(Out, OutSize, Path1, Path2, 0);
}

HRESULT
WINAPI
PathCchAppend(PWSTR Path, SIZE_T Size, PCWSTR More)
{
    return PathCchCombineEx(Path, Size, Path, More, 0);
}

HRESULT
WINAPI
PathCchAddBackslash(PWSTR Path, SIZE_T Size)
{
    SIZE_T Len;

    if (!Path || !Size || Size > PATHCCH_MAX_CCH) return E_INVALIDARG;
    Len = wcslen(Path);
    if (Len && PcIsSep(Path[Len - 1])) return S_FALSE;
    if (Len + 1 >= Size) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    Path[Len] = L'\\';
    Path[Len + 1] = UNICODE_NULL;
    return S_OK;
}

HRESULT
WINAPI
PathCchRemoveFileSpec(PWSTR Path, SIZE_T Size)
{
    SIZE_T Len, Pos, Min = 0;

    if (!Path || !Size || Size > PATHCCH_MAX_CCH) return E_INVALIDARG;
    Len = wcslen(Path);
    if (Len >= 2 && Path[1] == L':') Min = PcIsSep(Path[2]) ? 3 : 2;
    else if (Len >= 1 && PcIsSep(Path[0]) && !(Len >= 2 && PcIsSep(Path[1]))) Min = 1;
    Pos = Len;
    while (Pos > Min && !PcIsSep(Path[Pos - 1])) Pos--;
    if (Pos == Min) /* no separator past the root: remove everything after the root */
    {
        if (Len == Min) return S_FALSE;
        Path[Min] = UNICODE_NULL;
        return S_OK;
    }
    if (Pos > Min + 1 || Pos == Min + 1) Pos--;     /* drop the separator unless it is the root's */
    if (Pos < Min) Pos = Min;
    if (Pos == Len) return S_FALSE;
    Path[Pos] = UNICODE_NULL;
    return S_OK;
}

HRESULT
WINAPI
PathCchFindExtension(PCWSTR Path, SIZE_T Size, PCWSTR *Extension)
{
    PCWSTR End, Dot = NULL, p;

    if (!Path || !Size || Size > PATHCCH_MAX_CCH || !Extension) return E_INVALIDARG;
    for (End = Path; End < Path + Size && *End; End++) { }
    if (End == Path + Size) return E_INVALIDARG;
    for (p = Path; p < End; p++)
    {
        if (*p == L'.') Dot = p;
        else if (PcIsSep(*p) || *p == L' ') Dot = NULL;
    }
    *Extension = Dot ? Dot : End;
    return S_OK;
}
