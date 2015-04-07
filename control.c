#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "control.h"
#include "process.h"
#include "route.h"
#include "tilera.h"
#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
#define INFO(F, ...)


// Show example program command lines to run against this switch.
//
static void showTesterCommandLine(Thread *t, int fd)
{
    Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    const int size = netio_get(q, NETIO_PARAM, NETIO_PARAM_MAC,
                               p->forward.mac, sizeof p->forward.mac);
    if (size != sizeof p->forward.mac) {
        error("%02d: netio_get(%p, NETIO_PARAM, NETIO_PARAM_MAC, %p, %d) "
              "returned %d: %s",
              t->index, q, p->forward.mac, sizeof p->forward.mac,
              size, netio_strerror(size));
    }
    const unsigned char *const fip = p->forward.ip;
    const unsigned char *const mac = p->forward.mac;
    const unsigned char *const cip = p->control.ip;
    INFO("%02d: controlRoutes(%p) listen fd %d on " IPFMT ":%d (" MACFMT ")",
         t->index, t, fd, i[0], i[1], i[2], i[3], CONTROLPORT,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    show("%02d: Listening for commands on TCP " IPFMT ":%d",
         t->index, cip[0], cip[1], cip[2], cip[3], CONTROLPORT);
    show("%02d: Run ./tester " IPFMT " %s " IPFMT " " MACFMT
         " <routes> <packets> seconds>",
         t->index, cip[0], cip[1], cip[2], cip[3],
         p->interface, fip[0], fip[1], fip[2], fip[3],
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    show("%02d: Or run ./driver " IPFMT " %d",
         t->index, cip[0], cip[1], cip[2], cip[3], CONTROLPORT);
    show("%02d: Send video UDP to " IPFMT " (" MACFMT ")",
         t->index, fip[0], fip[1], fip[2], fip[3],
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


// Read size bytes from fd into buffer.  Return size or -1 on EOF.
//
static int readControlStuff(int fd, char *buffer, size_t size)
{
    const int result = size;
    buffer[0] = ""[0];                  // Nul out the buffer.
    char *p = buffer;
    while (size > 0) {
        const int rSize = read(fd, p, size);
        if (rSize < 0) {
            error("__: readControlStuff(%d) read(%d, %p, %zu) returned %d "
                  "with errno %d: %s", fd, fd, p, size, rSize,
                  errno, strerror(errno));
            return -1;
        } else if (rSize == 0) {
            info("__:  readControlStuff(%d) "
                 "read(%d, %p, %zu) returned 0 for EOF",
                 fd, fd, p, size);
            return -1;
        } else {
            p += rSize;
            size -= rSize;
        }
    }
    return result;
}


// Read and handle route control strings from fd.  Return 1 on EOF or error.
//
static int handleOneRoute(Thread *t, int fd)
{
    info("%02d: handleOneRoute(%p, %d)", t->index, t, fd);
    char buffer[999];
    int size = -1;
    const int sizeSize = readControlStuff(fd, (char *)&size, sizeof size);
    if (sizeSize == sizeof size) {
        if (size == 0) {
            INFO("%02d: readControlStuff(%d, %p, %zu) got EOF",
                 t->index, fd, &size, sizeof size);
        } else if (size > sizeof buffer) {
            error("%02d: readControlStuff(%d, %p, %zu) got size %d",
                  t->index, fd, &size, sizeof size, size);
        } else {
            const ssize_t rtSize = read(fd, buffer, size);
            if (rtSize == size) {
                info("%02d: readControlStuff(%d, %p, %zu) got:\n%s",
                     t->index, fd, buffer, size, buffer);
                const Route rt = routeFromString(buffer);
                if (rt.dst.port > 0) routeOpen(&rt); else routeClose(&rt);
                ++t->process->routeCount;
                return 0;
            } else {
                error("%02d: readControlStuff(%d, %p, %zu) returned %zd",
                      t->index, fd, buffer, size, rtSize);
            }
        }
    }
    return 1;
}


int controlRoutes(struct Thread *t)
{
    INFO("%02d: controlRoutes(%p)", t->index, t);
    Process *const p = t->process;
    const int listenFd = listenTcpPort("0.0.0.0", CONTROLPORT);
    showTesterCommandLine(t, listenFd);
    struct sockaddr_in address;
    socklen_t addressSize = sizeof address;
    const int acceptFd =
        accept(listenFd, (struct sockaddr *)&address, &addressSize);
    if (acceptFd == -1) {
        error("%02d: accept(%d, %p, %p) returned %d with errno %d: %s",
              t->index, listenFd, &address, &addressSize, acceptFd,
              errno, strerror(errno));
    } else {
        int done = 0;
        while (!done) done = handleOneRoute(t, acceptFd);
    }
    close(acceptFd);
    close(listenFd);
    return p->routeCount;
}
