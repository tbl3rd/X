#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "route.h"
#include "util.h"


static const char usage[] =
    "                                                                     \n"
    "%s: Send route commands from stdin to a UDP switch at <ip>:<port>.   \n"
    "    You can build and run %s on any Unix system because id does not  \n"
    "    depend on Tilera libraries.                                      \n"
    "                                                                     \n"
    "Usage: %s <ip> [<port>]                                              \n"
    "                                                                     \n"
    "Where: <ip> is the dotted-decimal IPv4 address string for the        \n"
    "            command interface on the UDP switch.                     \n"
    "       <port> is the integer TCP port number for the command         \n"
    "              interface on the UDP switch.  The default is %d.       \n"
    "                                                                     \n"
    "Example: %s 172.17.3.126 %d                                          \n"
    "                                                                     \n";

// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Describe this program's validated command line.
//
typedef struct DriverCommandLine {
    const char *av0;
    const char *ips;
    unsigned char ip[4];
    int port;
} DriverCommandLine;

// Validate the command line (ac, av) and return the results.
//
static const DriverCommandLine validateDriverUsage(int ac, const char *av[])
{
    INFO("__: validateDriverUsage(%d, %p)", ac, av);
    const char *av0 = strrchr(av[0], "/"[0]); av0 = av0? 1 + av0: av[0];
    fprintf(stderr, "%s command line:", av0);
    for (int n = 0; n < ac; ++n) fprintf(stderr, " '%s'", av[n]);
    fprintf(stderr, "\n");
    DriverCommandLine result = { .av0 = av0, .port = CONTROLPORT };
    int ok = ac > 1 && validIpString(av[1]);
    if (ok) {
        result.ips = av[1];
        ok = ipFromString(result.ip, result.ips);
    }
    if (ok && ac > 2) {
        result.port = atoi(av[2]);
        ok = result.port > 0;
    }
    if (!ok) {
        fprintf(stderr, usage, av0, av0, av0, CONTROLPORT, av0, CONTROLPORT);
        exit(1);
    }
    return result;
}


// Write at r the next route described in the JSON stream s.
// Return 1 on EOF.  Otherwise return 0.
//
// Add lines from s to static buffer until its content can be successfully
// parsed into a route.  Maintain the size of the current string in sofar.
//
static int routeFromStream(Route *r, FILE *s)
{
    static char buffer[999];
    static size_t sofar;
    while (1) {
        assert(sofar < sizeof buffer);
        fgets(buffer + sofar, sizeof buffer - sofar, s);
        if (feof(s)) return 1;
        if (ferror(s)) {
            error("__: fgets(%p, %zu, %p) failed with errno %d: %s",
                  buffer + sofar, sizeof buffer - sofar, s,
                  errno, strerror(errno));
            clearerr(s);
        } else {
            int ip[4];
            unsigned int mac[6];
            const int count =
                sscanf(buffer, JSONROUTEFMT, &r->poa, &r->dst.port,
                       ip + 0, ip + 1, ip + 2, ip + 3,
                       mac + 0, mac + 1, mac + 2, mac + 3, mac + 4, mac + 5);
            if (count == 12) {
                for (int n = 0; n < 4; ++n) {
                    r->dst.ip[n]  = (unsigned char)ip[n];
                }
                for (int n = 0; n < 6; ++n) {
                    r->dst.mac[n] = (unsigned char)mac[n];
                }
                sofar = 0;
                return 0;
            }
            sofar = strlen(buffer);
        }
    }
    assert(0);
    return 1;
}


int main(int ac, const char *av[])
{
    INFO("__: main(%d, %p)", ac, av);
    const DriverCommandLine cl = validateDriverUsage(ac, av);
    errorInitialize(cl.av0);
    const int fd = connectTcpPort(cl.ips, cl.port);
    const int eofIsFail = setvbuf(stdin, NULL, _IOLBF, 0);
    INFO("__: setvbuf() returned %d where EOF is %d", eofIsFail, EOF);
    int done = 0;
    while (!done) {
        Route r = {};
        done = routeFromStream(&r, stdin);
        if (done) {
            stopSwitch(fd);
        } else {
            routeSendControl(fd, &r);
        }
    }
    return 0;
}
