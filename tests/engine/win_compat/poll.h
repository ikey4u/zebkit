/* <poll.h> on Windows, of a pipe's reading end. Not in win_compat.h, which every
 * test has: some define a function of this name. */

#ifndef ZEBKIT_TEST_WIN_COMPAT_POLL_H
#define ZEBKIT_TEST_WIN_COMPAT_POLL_H

#include "win_compat.h"

/* ---- poll, of a pipe's reading end ---- */
struct pollfd {
    int fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;
#define POLLIN 1
#define POLLERR 8
#define POLLHUP 16
WIN_COMPAT int poll(struct pollfd *fds, nfds_t count, int timeout) {
    const ULONGLONG end = GetTickCount64() + (timeout < 0 ? 0 : (ULONGLONG)timeout);
    for (;;) {
        int ready = 0;
        for (nfds_t i = 0; i < count; ++i) {
            DWORD available = 0;
            fds[i].revents = 0;
            if (!PeekNamedPipe((HANDLE)_get_osfhandle(fds[i].fd), NULL, 0, NULL, &available, NULL))
                fds[i].revents = POLLHUP;
            else if (available)
                fds[i].revents = POLLIN;
            if (fds[i].revents)
                ++ready;
        }
        if (ready || (timeout >= 0 && GetTickCount64() >= end))
            return ready;
        Sleep(1);
    }
}

#endif /* ZEBKIT_TEST_WIN_COMPAT_POLL_H */
