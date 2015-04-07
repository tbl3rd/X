#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "route.h"
#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
#define INFO(F, ...) info(F, ## __VA_ARGS__)
// #define INFO(F, ...)


static Route route[R30TOTALCHANNELS];
static int routeLimit = sizeof route / sizeof route[0];


void routeInitialize(void)
{
    INFO("__: routeInitialize()");
    for (int n = 0; n < routeLimit; ++n) {
        const Route rtN = { .index = n, .poa = PORTOFFSET + n };
        route[n] = rtN;
    }
}


void routeOpen(const Route *r)
{
    INFO("__: routeOpen(%p)", r);
    const int index = r->poa - PORTOFFSET;
    assert(index >= 0 && index < routeLimit);
    route[index].dst = r->dst;
    route[index].open = 1;
}


void routeClose(const Route *r)
{
    INFO("__: routeClose(%p)", r);
    const int index = r->poa - PORTOFFSET;
    const int ok = index >= 0 && index < routeLimit;
    if (!ok) {
        error("__: routeClose(%p) index is %d", r, index);
        assert(ok);
    }
    if (r->poa == route[index].poa) {
        const unsigned char *const i = route[index].dst.ip;
        const unsigned char *const m = route[index].dst.mac;
        INFO("__: Close route %d to " IPFMT ":%d (" MACFMT ")",
             route[index].poa, i[0], i[1], i[2], i[3], route[index].dst.port,
             m[0], m[1], m[2], m[3], m[4], m[5]);
        route[index].open = 0;
    }
}


const Route routeFromPortOfArrival(int poa)
{
    // INFO("__: routeFromPortOfArrival(%d)", poa); // too much spew
    const int index = poa - PORTOFFSET;
    const int ok = index >= 0 && index < routeLimit;
    if (ok) {
        if (poa == route[index].poa) return route[index];
    } else {
        error("__: routeFromPortOfArrival(%d) index is %d", poa, index);
    }
    const Route badRoute = { .index = -1, .poa = poa };
    return badRoute;
}


// A route string with only the from port set closes the route.
//
const Route routeFromString(const char *s)
{
    static const Endpoint dst = { .port = -1 };
    Route result = { .index = -1, .poa = -1, .dst = dst };
    int ip[4];
    unsigned int mac[6];
    const int count =
        sscanf(s, JSONROUTEFMT, &result.poa, &result.dst.port,
               ip  + 0, ip  + 1, ip  + 2, ip  + 3,
               mac + 0, mac + 1, mac + 2, mac + 3, mac + 4, mac + 5);
    if (count == 12) {
        for (int n = 0; n < 4; ++n) result.dst.ip[n]  = (unsigned char)ip[n];
        for (int n = 0; n < 6; ++n) result.dst.mac[n] = (unsigned char)mac[n];
    } else if (count == 1) {
        assert(result.poa >= PORTOFFSET && result.poa < routeLimit);
        result.dst.port = -1;
    } else {
        error("__: Cannot parse: %s with " JSONROUTEFMT, s);
    }
    return result;
}


// Write into buffer up to size bytes of a JSON string describing route.
// Return -1 or a count of the bytes written at buffer.
//
int routeToString(const Route *r, char *buffer, size_t size)
{
    const unsigned char *const i = r->dst.ip;
    const unsigned char *const m = r->dst.mac;
    const int count =
        snprintf(buffer, size, JSONROUTEFMT, r->poa, r->dst.port,
                 i[0], i[1], i[2], i[3], m[0], m[1], m[2], m[3], m[4], m[5]);
    buffer[size - 1] = ""[0];
    const int ok = count > 0 && count < size;
    if (ok) return count + 1;
    return -1;
}


// Send route at r on fd.
//
void routeSendControl(int fd, const Route *r)
{
    INFO("__: routeSendControl(%d, %p) for port %d", fd, r, r->poa);
    char buffer[999];
    const int size = routeToString(r, buffer, sizeof buffer);
    if (size > 0) {
        INFO("__: routeSendControl(%d, %p) sending:\n%s", fd, r, buffer);
        const int sSize = write(fd, &size, sizeof size);
        if (sSize == sizeof size) {
            const int rtSize = write(fd, buffer, size);
            if (size != rtSize) {
                error("__: write(%d, %p, %d) returned %d with errno %d: %s",
                      fd, buffer, size, rtSize, errno, strerror(errno));
            }
        } else {
            error("__: write(%d, %p, %d) returned %d with errno %d: %s",
                  fd, &size, sizeof size, sSize, errno, strerror(errno));
        }
    }
}
