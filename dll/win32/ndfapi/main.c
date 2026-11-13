/*
 * PROJECT:     ReactOS Network Diagnostics Framework API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal ndfapi: network diagnostics are not available.
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

HRESULT WINAPI NdfCloseIncident(PVOID Handle)
{
    UNREFERENCED_PARAMETER(Handle);
    return S_OK;
}

HRESULT WINAPI NdfCreateWebIncident(LPCWSTR Url, PVOID *Handle)
{
    UNREFERENCED_PARAMETER(Url);
    if (Handle) *Handle = NULL;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

HRESULT WINAPI NdfExecuteDiagnosis(PVOID Handle, HWND Window)
{
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Window);
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
