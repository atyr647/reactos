/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Mandatory integrity label ACEs
 */

#include <advapi32.h>

#define K32_SYSTEM_MANDATORY_LABEL_ACE_TYPE 0x11
#define K32_MANDATORY_POLICY_MASK           0x7

typedef struct _MANDATORY_LABEL_ACE_LAYOUT
{
    UCHAR AceType;
    UCHAR AceFlags;
    USHORT AceSize;
    ACCESS_MASK Mask;
    DWORD SidStart;
} MANDATORY_LABEL_ACE_LAYOUT;

/*
 * @implemented
 */
BOOL
WINAPI
AddMandatoryAce(IN OUT PACL pAcl,
                IN DWORD dwAceRevision,
                IN DWORD dwAceFlags,
                IN DWORD MandatoryPolicy,
                IN PSID pLabelSid)
{
    MANDATORY_LABEL_ACE_LAYOUT *Ace;
    DWORD SidLength, AceSize;
    BOOL Result;

    if (!pAcl || !pLabelSid || !IsValidSid(pLabelSid) ||
        (MandatoryPolicy & ~K32_MANDATORY_POLICY_MASK) ||
        dwAceRevision < ACL_REVISION)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SidLength = GetLengthSid(pLabelSid);
    AceSize = FIELD_OFFSET(MANDATORY_LABEL_ACE_LAYOUT, SidStart) + SidLength;
    Ace = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, AceSize);
    if (!Ace)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    Ace->AceType = K32_SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    Ace->AceFlags = (UCHAR)dwAceFlags;
    Ace->AceSize = (USHORT)AceSize;
    Ace->Mask = MandatoryPolicy;
    CopySid(SidLength, (PSID)&Ace->SidStart, pLabelSid);

    Result = AddAce(pAcl, dwAceRevision, MAXDWORD, Ace, AceSize);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Ace);
    return Result;
}
