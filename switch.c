#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "forward.h"
#include "process.h"
#include "route.h"
#include "tap.h"
#include "tilera.h"
#include "util.h"


static const char usage[] =
    "                                                                     \n"
    "%s: Forward UDP packets from input ports to remote addresses         \n"
    "    according to route commands sent to the control port %d.         \n"
    "                                                                     \n"
    "Usage: %s <fip> <fif>                                                \n"
    "                                                                     \n"
    "Where: <fip> is the IP address on which the switch forwards UDP      \n"
    "             packets.  (Send video to <fip> in other words.)         \n"
    "                                                                     \n"
    "       <fif> is the name of the network interface to use for UDP     \n"
    "             forwarding.  Usually '%s' or '%s'.  Use '%s' in         \n"
    "             production, but '%s' can avoid optical cabling.         \n"
    "                                                                     \n"
    "Each route command is a JSON string preceeded by its length encoded  \n"
    "as 4 bytes of binary.  The route command maps an input 'from' port   \n"
    "at address <fip> to an output 'port', 'ip', and 'mac' triple.        \n"
    "                                                                     \n"
    "%s\n"
    "To open a route choose a port number 'from' in [%d,%d) and set the   \n"
    "appropriate destination 'port' number and 'ip' and 'mac' addresses.  \n"
    "UDP packets arriving on <fip> and port 'from' are forwarded to the   \n"
    "'port' and 'ip' and 'mac' addresses specified in the route.          \n"
    "                                                                     \n"
    "To close a route, specify its 'from' port and set -1 as the route's  \n"
    "destination 'port'.                                                  \n"
    "                                                                     \n"
    "Example: %s %s %s\n"
    "\n";


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Describe this program's validated command line.
//
typedef struct SwitchCommandLine {
    const char *av0;
    const char *fif;
    const char *fip;
} SwitchCommandLine;

// Validate the command line (ac, av) and return the results.
//
static const SwitchCommandLine validateSwitchUsage(int ac, const char *av[])
{
    INFO("__: validateSwitchUsage(%d, %p)", ac, av);
    const char *av0 = strrchr(av[0], "/"[0]); av0 = av0? 1 + av0: av[0];
    fprintf(stderr, "%s command line:", av0);
    for (int n = 0; n < ac; ++n) fprintf(stderr, " '%s'", av[n]);
    fprintf(stderr, "\n");
    const int ok = ac == 3 && validIpString(av[1]) &&
        ((0 == strcmp(av[2], PRODUCTIONINTERFACE)) ||
         (0 == strcmp(av[2], CONVENIENCEINTERFACE)));
    if (!ok) {
        fprintf(stderr, usage, av0, CONTROLPORT, av0,
                PRODUCTIONINTERFACE, CONVENIENCEINTERFACE,
                PRODUCTIONINTERFACE, CONVENIENCEINTERFACE,
                JSONROUTEFMT, PORTOFFSET, CONTROLPORT,
                av0, EXAMPLEFORWARDINGIP, PRODUCTIONINTERFACE);
        exit(1);
    }
    const SwitchCommandLine result = {
        .av0 = av0, .fip = av[1], .fif = av[2]
    };
    return result;
}


int main(int ac, const char *av[])
{
    INFO("__: main(%d, %p", ac, av);
    const SwitchCommandLine cl = validateSwitchUsage(ac, av);
    errorInitialize(cl.av0);
    routeInitialize();
    Process *const p = processInitialize(cl.av0, forwardStart, "forwardStart");
    p->interface = cl.fif;
    ipFromString(p->forward.ip, cl.fip);
    Thread *const t = p->thread + 0;
    registerQueueReadWrite(p->thread + 0);
    initializeNetio(p);
    tapConfigure(p);
    int starts = processStartThreads(p, tapStart, "tapStart");
    starts += processStartThreads(p, forwardStart, "forwardStart");
    INFO("__: Started %d threads", starts);
    controlRoutes(p->thread + 0);
    const int stops = processStopThreads(p, forwardStart, "forwardStart");
    INFO("__: Stopped %d of %d threads", stops, starts);
    showCounters(p);
    unregisterQueue(t);
    const int status = 0;
    INFO("__: Exiting with status %d", status);
    processUninitialize(p);
    return status;
}
