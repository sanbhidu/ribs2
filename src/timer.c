/*
    This file is part of RIBS2.0 (Robust Infrastructure for Backend Systems).
    RIBS is an infrastructure for building great SaaS applications (but not
    limited to).

    Copyright (C) 2012 Adap.tv, Inc.

    RIBS is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, version 2.1 of the License.

    RIBS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with RIBS.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "timer.h"
#include "context.h"
#include "epoll_worker.h"
#include "logger.h"
#include <sys/timerfd.h>

static void _ribs_timer_wrapper(void) {
    void (**handler)(void) = (void (**)(void))current_ctx->reserved;
    for (;;yield()) {
        uint64_t num_exp;
        if (sizeof(num_exp) != read(last_epollev.data.fd, &num_exp, sizeof(num_exp))) {
            LOGGER_ERROR("size mismatch when reading from timerfd: %d", last_epollev.data.fd);
            continue;
        }
        (*handler)();
    }
}

int ribs_timer(time_t msec, void (*handler)(void)) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (0 > tfd)
        return LOGGER_PERROR("timerfd"), -1;
    struct ribs_context *ctx = ribs_context_create(1024 * 1024, sizeof(void (*)(void)), _ribs_timer_wrapper);
    void (**ref)(void) = (void (**)(void))ctx->reserved;
    *ref = handler;
    if (0 > ribs_epoll_add(tfd, EPOLLIN, ctx))
        return LOGGER_PERROR("epoll_add"), close(tfd), -1;
    struct itimerspec timerspec = {{msec/1000,(msec % 1000)*1000000},{msec/1000,(msec % 1000)*1000000}};
    if (0 > timerfd_settime(tfd, 0, &timerspec, NULL))
        return LOGGER_PERROR("timerfd_settime: %d", tfd), close(tfd), -1;
    return 0;
}
