/*
 * PROJECT:     ReactOS Windows Event Log API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal wevtapi: there are no Vista-style event channels, so queries find nothing.
 *
 * Applications such as Chromium delay-load these functions to peek at event logs and
 * cope with "channel not found"; having the DLL present is what keeps them running.
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

#ifndef ERROR_EVT_CHANNEL_NOT_FOUND
#define ERROR_EVT_CHANNEL_NOT_FOUND 15007L
#endif

typedef HANDLE EVT_HANDLE;

BOOL WINAPI EvtClose(EVT_HANDLE Object)
{
    UNREFERENCED_PARAMETER(Object);
    return TRUE;
}

EVT_HANDLE WINAPI EvtCreateRenderContext(DWORD ValuePathsCount, PVOID ValuePaths, DWORD Flags)
{
    UNREFERENCED_PARAMETER(ValuePathsCount);
    UNREFERENCED_PARAMETER(ValuePaths);
    UNREFERENCED_PARAMETER(Flags);
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

BOOL WINAPI EvtNext(EVT_HANDLE ResultSet, DWORD EventsSize, PVOID Events,
                    DWORD Timeout, DWORD Flags, PDWORD Returned)
{
    UNREFERENCED_PARAMETER(ResultSet);
    UNREFERENCED_PARAMETER(EventsSize);
    UNREFERENCED_PARAMETER(Events);
    UNREFERENCED_PARAMETER(Timeout);
    UNREFERENCED_PARAMETER(Flags);
    if (Returned) *Returned = 0;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

EVT_HANDLE WINAPI EvtOpenChannelEnum(EVT_HANDLE Session, DWORD Flags)
{
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(Flags);
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

EVT_HANDLE WINAPI EvtOpenPublisherEnum(EVT_HANDLE Session, DWORD Flags)
{
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(Flags);
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

EVT_HANDLE WINAPI EvtQuery(EVT_HANDLE Session, LPCWSTR Path, LPCWSTR Query, DWORD Flags)
{
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(Path);
    UNREFERENCED_PARAMETER(Query);
    UNREFERENCED_PARAMETER(Flags);
    SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
    return NULL;
}

BOOL WINAPI EvtRender(EVT_HANDLE Context, EVT_HANDLE Fragment, DWORD Flags, DWORD BufferSize,
                      PVOID Buffer, PDWORD BufferUsed, PDWORD PropertyCount)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Fragment);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(Buffer);
    if (BufferUsed) *BufferUsed = 0;
    if (PropertyCount) *PropertyCount = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

EVT_HANDLE WINAPI EvtSubscribe(EVT_HANDLE Session, HANDLE SignalEvent, LPCWSTR ChannelPath,
                               LPCWSTR Query, EVT_HANDLE Bookmark, PVOID Context,
                               PVOID Callback, DWORD Flags)
{
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(SignalEvent);
    UNREFERENCED_PARAMETER(ChannelPath);
    UNREFERENCED_PARAMETER(Query);
    UNREFERENCED_PARAMETER(Bookmark);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Flags);
    SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
    return NULL;
}
