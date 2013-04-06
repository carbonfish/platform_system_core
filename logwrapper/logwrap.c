/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <pthread.h>

#include <logwrap/logwrap.h>
#include "private/android_filesystem_config.h"
#include "cutils/log.h"

#define ARRAY_SIZE(x)   (sizeof(x) / sizeof(*(x)))

static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;

#define ERROR(fmt, args...)                                                   \
do {                                                                          \
    fprintf(stderr, fmt, ## args);                                            \
    ALOG(LOG_ERROR, "logwrapper", fmt, ## args);                              \
} while(0)

#define FATAL_CHILD(fmt, args...)                                             \
do {                                                                          \
    ERROR(fmt, ## args);                                                      \
    _exit(-1);                                                                \
} while(0)

static int parent(const char *tag, int parent_read, pid_t pid, int *chld_sts,
        bool logwrap) {
    int status = 0;
    char buffer[4096];
    struct pollfd poll_fds[] = {
        [0] = {
            .fd = parent_read,
            .events = POLLIN,
        },
    };
    int rc = 0;

    int a = 0;  // start index of unprocessed data
    int b = 0;  // end index of unprocessed data
    int sz;
    bool found_child = false;

    char *btag = basename(tag);
    if (!btag) btag = (char*) tag;

    while (!found_child) {
        if (TEMP_FAILURE_RETRY(poll(poll_fds, ARRAY_SIZE(poll_fds), -1)) < 0) {
            ERROR("poll failed\n");
            rc = -1;
            goto err_poll;
        }

        if (poll_fds[0].revents & POLLIN) {
            sz = read(parent_read, &buffer[b], sizeof(buffer) - 1 - b);

            sz += b;
            // Log one line at a time
            for (b = 0; b < sz; b++) {
                if (buffer[b] == '\r') {
                    buffer[b] = '\0';
                } else if (buffer[b] == '\n') {
                    buffer[b] = '\0';
                    if (logwrap)
                        ALOG(LOG_INFO, btag, "%s", &buffer[a]);
                    a = b + 1;
                }
            }

            if (a == 0 && b == sizeof(buffer) - 1) {
                // buffer is full, flush
                buffer[b] = '\0';
                if (logwrap)
                    ALOG(LOG_INFO, btag, "%s", &buffer[a]);
                b = 0;
            } else if (a != b) {
                // Keep left-overs
                b -= a;
                memmove(buffer, &buffer[a], b);
                a = 0;
            } else {
                a = 0;
                b = 0;
            }
        }

        if (poll_fds[0].revents & POLLHUP) {
            int ret;

            ret = waitpid(pid, &status, WNOHANG);
            if (ret < 0) {
                rc = errno;
                ALOG(LOG_ERROR, "logwrap", "waitpid failed with %s\n", strerror(errno));
                goto err_waitpid;
            }
            if (ret > 0) {
                found_child = true;
            }
        }
    }

    if (chld_sts != NULL) {
        *chld_sts = status;
    } else {
      if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
      else
        rc = -ECHILD;
    }

    if (logwrap) {
      // Flush remaining data
      if (a != b) {
        buffer[b] = '\0';
        ALOG(LOG_INFO, btag, "%s", &buffer[a]);
      }
      if (WIFEXITED(status)) {
        if (WEXITSTATUS(status))
          ALOG(LOG_INFO, "logwrapper", "%s terminated by exit(%d)", btag,
               WEXITSTATUS(status));
      } else {
        if (WIFSIGNALED(status))
          ALOG(LOG_INFO, "logwrapper", "%s terminated by signal %d", btag,
               WTERMSIG(status));
        else if (WIFSTOPPED(status))
          ALOG(LOG_INFO, "logwrapper", "%s stopped by signal %d", btag,
               WSTOPSIG(status));
      }
    }

err_waitpid:
err_poll:
    return rc;
}

static void child(int argc, char* argv[], bool logwrap) {
    // create null terminated argv_child array
    char* argv_child[argc + 1];
    memcpy(argv_child, argv, argc * sizeof(char *));
    argv_child[argc] = NULL;

    if (execvp(argv_child[0], argv_child)) {
        FATAL_CHILD("executing %s failed: %s\n", argv_child[0],
                strerror(errno));
    }
}

int android_fork_execvp(int argc, char* argv[], int *status, bool ignore_int_quit,
        bool logwrap) {
    pid_t pid;
    int parent_ptty;
    int child_ptty;
    char *child_devname = NULL;
    struct sigaction intact;
    struct sigaction quitact;
    sigset_t blockset;
    sigset_t oldset;
    int rc = 0;

    rc = pthread_mutex_lock(&fd_mutex);
    if (rc) {
        ERROR("failed to lock signal_fd mutex\n");
        goto err_lock;
    }

    /* Use ptty instead of socketpair so that STDOUT is not buffered */
    parent_ptty = open("/dev/ptmx", O_RDWR);
    if (parent_ptty < 0) {
        ERROR("Cannot create parent ptty\n");
        rc = -1;
        goto err_open;
    }

    if (grantpt(parent_ptty) || unlockpt(parent_ptty) ||
            ((child_devname = (char*)ptsname(parent_ptty)) == 0)) {
        ERROR("Problem with /dev/ptmx\n");
        rc = -1;
        goto err_ptty;
    }

    child_ptty = open(child_devname, O_RDWR);
    if (child_ptty < 0) {
        ERROR("Cannot open child_ptty\n");
        rc = -1;
        goto err_child_ptty;
    }

    sigemptyset(&blockset);
    sigaddset(&blockset, SIGINT);
    sigaddset(&blockset, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &blockset, &oldset);

    pid = fork();
    if (pid < 0) {
        close(child_ptty);
        ERROR("Failed to fork\n");
        rc = -1;
        goto err_fork;
    } else if (pid == 0) {
        pthread_mutex_unlock(&fd_mutex);
        pthread_sigmask(SIG_SETMASK, &oldset, NULL);
        close(parent_ptty);

        // redirect stdout and stderr
        dup2(child_ptty, 1);
        dup2(child_ptty, 2);
        close(child_ptty);

        child(argc, argv, logwrap);
    } else {
        close(child_ptty);
        if (ignore_int_quit) {
            struct sigaction ignact;

            memset(&ignact, 0, sizeof(ignact));
            ignact.sa_handler = SIG_IGN;
            sigaction(SIGINT, &ignact, &intact);
            sigaction(SIGQUIT, &ignact, &quitact);
        }

        rc = parent(argv[0], parent_ptty, pid, status, logwrap);
    }

    if (ignore_int_quit) {
        sigaction(SIGINT, &intact, NULL);
        sigaction(SIGQUIT, &quitact, NULL);
    }
err_fork:
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
err_child_ptty:
err_ptty:
    close(parent_ptty);
err_open:
    pthread_mutex_unlock(&fd_mutex);
err_lock:
    return rc;
}