/*
 * Copyright 2021 Yonatan Goldschmidt
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <stdbool.h>
#include <sched.h>

static int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
               int cpu, int group_fd, unsigned long flags)
{
   return syscall(__NR_perf_event_open, hw_event, pid, cpu,
                  group_fd, flags);
}

static ssize_t send_fd(int fd, void *buf, size_t len, int sendfd)
{
    // based on the example from 'man 3 cmsg'
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    struct iovec io = {
       .iov_base = buf,
       .iov_len = len,
    };
    union {         /* Ancillary data buffer, wrapped in a union
                      in order to ensure it is suitably aligned */
       char buf[CMSG_SPACE(sizeof(sendfd))];
       struct cmsghdr align;
    } u;

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sendfd));
    memcpy(CMSG_DATA(cmsg), &sendfd, sizeof(sendfd));

    return sendmsg(fd, &msg, 0);
}

// static bool enter_pid_and_net(pid_t pid) {
//     // TODO share code with enter_mount_ns and remove dup here
//     char path_net[128];
//     snprintf(path_net, sizeof(path_net), "/proc/%d/ns/net", pid);
//     char path_pid[128];
//     snprintf(path_pid, sizeof(path_pid), "/proc/%d/ns/pid", pid);

//     // TODO: for the PoC assume it's we're not the same NS
//     // TODO: keep old fds so we can exit afterwards.
//     int new_net = open(path_net, O_RDONLY);
//     int new_pid = open(path_pid, O_RDONLY);
//     int ret1 = syscall(__NR_setns, new_net, CLONE_NEWNET);
//     int ret2 = syscall(__NR_setns, new_pid, CLONE_NEWPID);
//     close(new_net);
//     close(new_pid);

//     return ret1 == 0 && ret2 == 0;
// }

static bool send_perf_fds(pid_t nspid) {
    printf("Opening for %d\n", nspid);

    // // we'll enter its PID ns (& net ns to find the UDS)
    // if (!enter_pid_and_net(pid)) {
    //     printf("failed to change ns\n");
    //     return false;
    // }

    // connect to its listening socket (possibly wait a bit for the socket)

    // TODO matching logic in PerfEvents::getFdForTid
    char path[108];
    snprintf(path + 1, sizeof(path) - 1, "async-profiler-%d", nspid);
    path[0] = '\0';
    struct sockaddr_un sun = {
        .sun_family = AF_UNIX,
    };
    memcpy(sun.sun_path, path, sizeof(sun.sun_path));

    const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket()");
        return false;
    }

    int i;
    for (i = 0; ; i++) {
        const socklen_t addrlen = sizeof(sun) - (sizeof(sun.sun_path) - (strlen(path + 1) + 1));
        printf("connect addrlen %d\n", addrlen);
        if (-1 == connect(sock, (const struct sockaddr *)&sun, addrlen)) {
            if (errno == ECONNREFUSED) {
                if (i == 10) {
                    printf("waited 1s.. still refused\n");
                    return false;
                } else {
                    // sleep 100ms
                    printf("sleeping 100ms\n");
                    struct timespec ts = {0, 100000};
                    nanosleep(&ts, NULL);
                }
            } else {
                perror("connect() to AP UDS");
                return false;
            }
        } else {
            break;
        }
    }

    printf("connected!\n");

    while (1) {
        // get next requested tid
        int tid;

        ssize_t ret = recv(sock, &tid, sizeof(tid), 0);
        if (ret == 0) {
            // done
            printf("got 0, done\n");
            break;
        } else if (ret != sizeof(tid)) {
            // TODO handle incomplete reads.
            perror("recv()");
            return false;
        }

        printf("sending for tid %d\n", tid);

        // TODO validate 'tid' is indeed a thread of 'nspid'

        // TODO handle different PerfEvent types in this protocol, currently I handle only "cpu".
        struct perf_event_attr attr = {0};
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
        attr.precise_ip = 2;
        attr.sample_period = 10000000; // DEFAULT_INTERVAL
        attr.sample_type = PERF_SAMPLE_CALLCHAIN;
        attr.disabled = 1;
        attr.wakeup_events = 1;

        int fd = syscall(__NR_perf_event_open, &attr, tid, -1, -1, 0);
        if (fd < 0) {
            perror("perf_event_open()");
            return false;
        }

        printf("for tid %d sending fd %d\n", tid, fd);

        if (send_fd(sock, &tid, sizeof(tid), fd) != sizeof(tid)) {
            perror("sendmsg()");
            return false;
        }
    }

    return true;
}

// run me with "nsenter -t PID -n -p ./send_fds nspid"
// not using setns(CLONE_NEWPID) because that requires an additional fork().
int main(int argc, const char *argv[]) {
    if (argc != 2) {
        return 1;
    }

    return send_perf_fds(atoi(argv[1])) ? 0 : 1;
}
