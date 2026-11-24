/* libdrm-ros extensions: what Mesa needs on ReactOS where Linux code would stat() a /dev node. */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "\\.\LKPIDRM<n>" for DRM minor n */
void drmRosNodeName(int minor, char *buf, size_t len);

/* The Linux device number (major 226, minor) of a node name from drmDevice.nodes[]: what stat().st_rdev gives on
 * Linux. 0 or -EINVAL. */
int drmRosGetNodeDevid(const char *node, dev_t *devid);

#ifdef __cplusplus
}
#endif
