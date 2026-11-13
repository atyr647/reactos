/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared-resource / keyed-mutex D3DKMT entry points (not supported yet)
 *
 * D3D runtimes such as DXVK import these from gdi32. The emulation layer has no cross-process
 * sharing, so they only fail cleanly. Deliberately no d3dkmthk.h here: the signatures differ per
 * header version and every parameter is pointer-sized or smaller.
 */

#include <windef.h>
#include <winbase.h>

#define STATUS_NOT_SUPPORTED ((LONG)0xC00000BBL)

#define D3DKMT_NOT_SUPPORTED(Name) \
    LONG WINAPI Name(PVOID Args) { (void)Args; return STATUS_NOT_SUPPORTED; }

D3DKMT_NOT_SUPPORTED(D3DKMTOpenResource2)
D3DKMT_NOT_SUPPORTED(D3DKMTOpenResourceFromNtHandle)
D3DKMT_NOT_SUPPORTED(D3DKMTOpenSynchronizationObject)
D3DKMT_NOT_SUPPORTED(D3DKMTOpenSyncObjectFromNtHandle)
D3DKMT_NOT_SUPPORTED(D3DKMTOpenKeyedMutex)
D3DKMT_NOT_SUPPORTED(D3DKMTCreateKeyedMutex2)
D3DKMT_NOT_SUPPORTED(D3DKMTDestroyKeyedMutex)
D3DKMT_NOT_SUPPORTED(D3DKMTQueryResourceInfoFromNtHandle)

LONG WINAPI D3DKMTShareObjects(UINT Count, PVOID Handles, PVOID Attr, UINT Access, PVOID Shared)
{
    (void)Count; (void)Handles; (void)Attr; (void)Access; (void)Shared;
    return STATUS_NOT_SUPPORTED;
}
