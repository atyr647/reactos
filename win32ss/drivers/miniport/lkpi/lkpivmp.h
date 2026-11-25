/* LinuxKPI display miniport: lkpivmp_io.c (kernel headers) for lkpivmp.c (video miniport headers) */
#pragma once

LONG LkpiVmpOpenDisplay(PVOID *DisplayFile, PVOID *DisplayDevice);       /* NTSTATUS */
LONG LkpiVmpQueryDisplay(PVOID DisplayDevice, LKPI_DISPLAY_INFO *Info);  /* NTSTATUS */
