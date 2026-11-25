/*
 * PROJECT:     ReactOS user environment library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     AppContainer profile functions (not supported)
 */

#include "precomp.h"

/*
 * @implemented
 * AppContainers do not exist on ReactOS; callers get ERROR_NOT_SUPPORTED.
 */
HRESULT
WINAPI
CreateAppContainerProfile(IN PCWSTR pszAppContainerName,
                          IN PCWSTR pszDisplayName,
                          IN PCWSTR pszDescription,
                          IN PVOID pCapabilities,
                          IN DWORD dwCapabilityCount,
                          OUT PSID *ppSidAppContainerSid)
{
    if (ppSidAppContainerSid) *ppSidAppContainerSid = NULL;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

/*
 * @implemented
 */
HRESULT
WINAPI
DeriveAppContainerSidFromAppContainerName(IN PCWSTR pszAppContainerName,
                                          OUT PSID *ppsidAppContainerSid)
{
    if (ppsidAppContainerSid) *ppsidAppContainerSid = NULL;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
