/* libdrm-ros: device numbers as on Linux (DRM major 226; minor = node number), for drmGetDeviceFromDevId() and
 * VK_EXT_physical_device_drm. */
#pragma once
#include <sys/types.h>
#define major(dev)              ((unsigned int)(((dev) >> 8) & 0xfffU))
#define minor(dev)              ((unsigned int)(((dev) & 0xffU) | (((dev) >> 12) & 0xfff00U)))
#define makedev(ma, mi)         ((dev_t)((((ma) & 0xfffU) << 8) | ((mi) & 0xffU) | (((mi) & 0xfff00U) << 12)))
