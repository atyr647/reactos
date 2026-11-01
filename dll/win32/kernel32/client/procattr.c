/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Process and thread attribute lists (Windows Vista and later)
 *
 * The list is an opaque structure to callers. This keeps the same layout as
 * Windows and Wine so that sizes agree. CreateProcess does not act on the
 * attributes yet (for example the handle inheritance list or the parent
 * process): they are only stored and validated.
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

#define K32_ATTRIBUTE_NUMBER_MASK 0x0000ffff
#define K32_ATTRIBUTE_REPLACE_VALUE 0x00000001

typedef struct _K32_PROC_THREAD_ATTR
{
    DWORD_PTR Attribute;
    SIZE_T Size;
    PVOID Value;
} K32_PROC_THREAD_ATTR;

typedef struct _K32_PROC_THREAD_ATTRIBUTE_LIST
{
    DWORD Mask;      /* one bit per attribute number already present */
    DWORD Capacity;  /* number of entries the list was sized for */
    DWORD Count;     /* entries in use */
    DWORD Reserved;
    DWORD_PTR Unknown;
    K32_PROC_THREAD_ATTR Attrs[1];
} K32_PROC_THREAD_ATTRIBUTE_LIST;

/*
 * @implemented
 */
BOOL
WINAPI
InitializeProcThreadAttributeList(OUT PVOID lpAttributeList,
                                  IN DWORD dwAttributeCount,
                                  IN DWORD dwFlags,
                                  IN OUT PSIZE_T lpSize)
{
    K32_PROC_THREAD_ATTRIBUTE_LIST *List = lpAttributeList;
    SIZE_T Needed;

    if (!lpSize || dwFlags || !dwAttributeCount || dwAttributeCount > 32)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Needed = FIELD_OFFSET(K32_PROC_THREAD_ATTRIBUTE_LIST, Attrs) +
             dwAttributeCount * sizeof(K32_PROC_THREAD_ATTR);

    if (!List || *lpSize < Needed)
    {
        /* Size query, or a buffer that is too small */
        *lpSize = Needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    *lpSize = Needed;
    RtlZeroMemory(List, Needed);
    List->Capacity = dwAttributeCount;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
UpdateProcThreadAttribute(IN OUT PVOID lpAttributeList,
                          IN DWORD dwFlags,
                          IN DWORD_PTR Attribute,
                          IN PVOID lpValue,
                          IN SIZE_T cbSize,
                          OUT PVOID lpPreviousValue OPTIONAL,
                          OUT PSIZE_T lpReturnSize OPTIONAL)
{
    K32_PROC_THREAD_ATTRIBUTE_LIST *List = lpAttributeList;
    DWORD Number = (DWORD)(Attribute & K32_ATTRIBUTE_NUMBER_MASK);
    DWORD Bit, i;

    if (!List || (dwFlags & ~K32_ATTRIBUTE_REPLACE_VALUE) || lpPreviousValue || lpReturnSize ||
        !lpValue || !cbSize || Number >= 32)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Bit = 1u << Number;
    if (List->Mask & Bit)
    {
        if (!(dwFlags & K32_ATTRIBUTE_REPLACE_VALUE))
        {
            SetLastError(ERROR_OBJECT_NAME_EXISTS);
            return FALSE;
        }

        /* Replace the existing entry */
        for (i = 0; i < List->Count; i++)
        {
            if ((List->Attrs[i].Attribute & K32_ATTRIBUTE_NUMBER_MASK) == Number)
            {
                List->Attrs[i].Attribute = Attribute;
                List->Attrs[i].Size = cbSize;
                List->Attrs[i].Value = lpValue;
                return TRUE;
            }
        }
    }

    if (List->Count >= List->Capacity)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    List->Attrs[List->Count].Attribute = Attribute;
    List->Attrs[List->Count].Size = cbSize;
    List->Attrs[List->Count].Value = lpValue;
    List->Count++;
    List->Mask |= Bit;
    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
DeleteProcThreadAttributeList(IN OUT PVOID lpAttributeList)
{
    /* Nothing is allocated by the list itself */
}
