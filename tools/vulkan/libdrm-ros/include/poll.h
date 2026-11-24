/*
 * libdrm-ros: <poll.h> for libsync.h and Mesa's sync-file helpers. Sync files are Linux fds that the kernel side
 * cannot create here (no fd table), so there is never one to wait for: poll() fails with ENOSYS.
 */
#pragma once

#ifndef POLLIN                      /* <winsock2.h> may have defined these already */
#define POLLIN          0x0001
#define POLLPRI         0x0002
#define POLLOUT         0x0004
#define POLLERR         0x0008
#define POLLHUP         0x0010
#define POLLNVAL        0x0020
struct pollfd {
    int fd;
    short events;
    short revents;
};
#endif
typedef unsigned long nfds_t;

#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
#ifdef __cplusplus
}
#endif
