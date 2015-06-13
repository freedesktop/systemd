/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/file.h>

#include "util.h"
#include "lockfile-util.h"
#include "fileio.h"

int make_lock_file(const char *p, int operation, LockFile *ret) {
        _cleanup_close_ int fd = -1;
        _cleanup_free_ char *t = NULL;
        int r;

        /*
         * We use UNPOSIX locks if they are available. They have nice
         * semantics, and are mostly compatible with NFS. However,
         * they are only available on new kernels. When we detect we
         * are running on an older kernel, then we fall back to good
         * old BSD locks. They also have nice semantics, but are
         * slightly problematic on NFS, where they are upgraded to
         * POSIX locks, even though locally they are orthogonal to
         * POSIX locks.
         */

        t = strdup(p);
        if (!t)
                return -ENOMEM;

        for (;;) {
                struct flock fl = {
                        .l_type = (operation & ~LOCK_NB) == LOCK_EX ? F_WRLCK : F_RDLCK,
                        .l_whence = SEEK_SET,
                };
                struct stat st;

                fd = open(p, O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
                if (fd < 0)
                        return -errno;

                r = fcntl(fd, (operation & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl);
                if (r < 0) {

                        /* If the kernel is too old, use good old BSD locks */
                        if (errno == EINVAL)
                                r = flock(fd, operation);

                        if (r < 0)
                                return errno == EAGAIN ? -EBUSY : -errno;
                }

                /* If we acquired the lock, let's check if the file
                 * still exists in the file system. If not, then the
                 * previous exclusive owner removed it and then closed
                 * it. In such a case our acquired lock is worthless,
                 * hence try again. */

                r = fstat(fd, &st);
                if (r < 0)
                        return -errno;
                if (st.st_nlink > 0)
                        break;

                fd = safe_close(fd);
        }

        ret->path = t;
        ret->fd = fd;
        ret->operation = operation;

        fd = -1;
        t = NULL;

        return r;
}

int make_lock_file_for(const char *p, int operation, LockFile *ret) {
        const char *fn;
        char *t;

        assert(p);
        assert(ret);

        fn = basename(p);
        if (!filename_is_valid(fn))
                return -EINVAL;

        t = newa(char, strlen(p) + 2 + 4 + 1);
        stpcpy(stpcpy(stpcpy(mempcpy(t, p, fn - p), ".#"), fn), ".lck");

        return make_lock_file(t, operation, ret);
}

void release_lock_file(LockFile *f) {
        int r;

        if (!f)
                return;

        if (f->path) {

                /* If we are the exclusive owner we can safely delete
                 * the lock file itself. If we are not the exclusive
                 * owner, we can try becoming it. */

                if (f->fd >= 0 &&
                    (f->operation & ~LOCK_NB) == LOCK_SH) {
                        static const struct flock fl = {
                                .l_type = F_WRLCK,
                                .l_whence = SEEK_SET,
                        };

                        r = fcntl(f->fd, F_OFD_SETLK, &fl);
                        if (r < 0 && errno == EINVAL)
                                r = flock(f->fd, LOCK_EX|LOCK_NB);

                        if (r >= 0)
                                f->operation = LOCK_EX|LOCK_NB;
                }

                if ((f->operation & ~LOCK_NB) == LOCK_EX)
                        unlink_noerrno(f->path);

                free(f->path);
                f->path = NULL;
        }

        f->fd = safe_close(f->fd);
        f->operation = 0;
}