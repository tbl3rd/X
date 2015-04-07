#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "packets.h"
#include "process.h"
#include "route.h"
#include "tap.h"
#include "tilera.h"
#include "util.h"

#include <tmc/cpus.h>


static const char usage[] =
    "                                                                     \n"
    "%s: Send UDP packets to ports [%d, %d) on the remote addresses       \n"
    "    <fip> and <mac>, after sending routing commands to port %d       \n"
    "    on <cip> that set up the UDP switch to route the sent packets    \n"
    "    back to this program.                                            \n"
    "                                                                     \n"
    "Usage: %s <cip> <fif> <fip> <mac> <routes> <packets> <seconds>       \n"
    "                                                                     \n"
    "Where: <cip> is the IP address of the switch's control port.         \n"
    "             Send route control commands over TCP to <cip>:%d.       \n"
    "       <fif> is the name of the network interface to use for sending \n"
    "             and receiving packets.   Usually '%s' or '%s'.          \n"
    "             Use '%s' in production, but '%s' can avoid setting      \n"
    "             up an optical connection.                               \n"
    "       <fip> is the dotted-decimal IPv4 address string for the       \n"
    "             network interface on the switch that forwards UDP       \n"
    "             packet traffic.                                         \n"
    "       <mac> is the hex and colon ethernet MAC address string        \n"
    "             of the switch's forwarding interface at <fip>.          \n"
    "       <routes> is the number of forwarding routes to set up on      \n"
    "                the switch at <ip> and <mac>.  The default is %d.    \n"
    "       <packets> is the number of packets to send on each route.     \n"
    "                 The default is %d.                                  \n"
    "       <seconds> is the total number of seconds to wait for packets  \n"
    "                 returned from the UDP switch.  The default is %d.   \n"
    "                                                                     \n"
    "Example: %s 172.17.3.126 %s %s 2e:97:ef:aa:43:c2\n"
    "\n";

// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Describe this program's validated command line.
//
typedef struct TesterCommandLine {
    const char *av0;
    const char *cip;
    const char *fif;
    const char *fip;
    const char *mac;
    int routes;
    int packets;
    int seconds;
} TesterCommandLine;

// Validate the command line (ac, av) and return the results.
//
static const TesterCommandLine validateTesterUsage(int ac, const char *av[])
{
    INFO("__: validateTesterUsage(%d, %p)", ac, av);
    static const int defaultPackets = 9999;
    static const int defaultSeconds = 99;
    const char *av0 = strrchr(av[0], "/"[0]); av0 = av0? 1 + av0: av[0];
    TesterCommandLine result = { .av0 = av0 };
    fprintf(stderr, "%s command line:", av0);
    for (int n = 0; n < ac; ++n) fprintf(stderr, " '%s'", av[n]);
    fprintf(stderr, "\n");
    const int ok =
        ac > 4 &&
        validIpString( av[1]) &&
        validIpString( av[3]) &&
        validMacString(av[4]) &&
        ((0 == strcmp(av[2], PRODUCTIONINTERFACE)) ||
         (0 == strcmp(av[2], CONVENIENCEINTERFACE)));
    if (ok) {
        result.cip     = av[1];
        result.fif     = av[2];
        result.fip     = av[3];
        result.mac     = av[4];
        result.routes  = R30TOTALCHANNELS;
        result.packets = defaultPackets;
        result.seconds = defaultSeconds;
    } else {
        fprintf(stderr, usage, av0, PORTOFFSET, CONTROLPORT, CONTROLPORT,
                av0, CONTROLPORT, PRODUCTIONINTERFACE, CONVENIENCEINTERFACE,
                PRODUCTIONINTERFACE, CONVENIENCEINTERFACE,
                R30TOTALCHANNELS, defaultPackets, defaultSeconds,
                av0, PRODUCTIONINTERFACE, EXAMPLEFORWARDINGIP);
        exit(1);
    }
    if (ac > 5) {
        const int number = atoi(av[5]);
        if (number > 0 && number < R30TOTALCHANNELS) result.routes = number;
    }
    if (ac > 6) {
        const int number = atoi(av[6]);
        if (number > 0) result.packets = number;
    }
    if (ac > 7) {
        const int number = atoi(av[7]);
        if (number > 0) result.seconds = number;
    }
    return result;
}


// Send p->routeCount JSON open route control strings to UDP switch on fd.
//
static void startRoutes(Process *p, int fd)
{
    INFO("__: startRoutes(%d)", fd);
    for (int n = 0; n < p->routeCount; ++n) {
        Endpoint dst = { .port = PORTOFFSET + n };
        Route rt = {
            .index = n,
            .poa   = PORTOFFSET + n,
            .dst = dst
        };
        memcpy(rt.dst.ip,  p->forward.ip,  sizeof rt.dst.ip);
        memcpy(rt.dst.mac, p->forward.mac, sizeof rt.dst.mac);
        char buffer[999];
        routeToString(&rt, buffer, sizeof buffer);
        info("__: startRoutes(%d) opening route:\n%s", fd, buffer);
        routeOpen(&rt);
        routeSendControl(fd, &rt);
    }
}


// Send count JSON close route control strings to UDP switch on fd.
// Then stop the switch.
//
static void stopRoutes(Process *p, int fd)
{
    static const Endpoint closedEndpoint = {};
    INFO("__: stopRoutes(%d)", fd);
    for (int n = 0; n < p->routeCount; ++n) {
        const int poa = PORTOFFSET + n;
        const Route rt = routeFromPortOfArrival(poa);
        routeClose(&rt);
        Route closeRoute = rt;
        closeRoute.dst = closedEndpoint;
        routeSendControl(fd, &closeRoute);
        SLEEP(1);
    }
    stopSwitch(fd);
}


int main(int ac, const char *av[])
{
    INFO("__: main(%d, %p)", ac, av);
    const TesterCommandLine cl = validateTesterUsage(ac, av);
    errorInitialize(cl.av0);
    routeInitialize();
    Process *const p = processInitialize(cl.av0, packetsStart, "packetsStart");
    p->interface = cl.fif;
    Thread *const t = p->thread + 0;
    ipFromString(p->forward.ip, cl.fip);
    macFromString(p->forward.mac, cl.mac);
    p->routeCount = cl.routes;
    p->packetCount = cl.packets;
    const int fd = connectTcpPort(cl.cip, CONTROLPORT);
    registerQueueReadWrite(p->thread + 0);
    initializeNetio(p);
    tapConfigure(p);
    startRoutes(p, fd);
    SLEEP(1);
    int starts = processStartThreads(p, tapStart, "tapStart");
    starts += processStartThreads(p, packetsStart, "packetsStart");
    INFO("__: Started %d threads", starts);
    packetsPrimePipeline(t);
    INFO("__: main() sleep(%d)", cl.seconds);
    sleep(cl.seconds);
    const int stops = processStopThreads(p, packetsStart, "packetsStart");
    INFO("__: Stopped %d of %d threads", stops, starts);
    stopRoutes(p, fd);
    showCounters(p);
    unregisterQueue(t);
    const int status = 0;
    INFO("__: Exiting with status %d", status);
    processUninitialize(p);
    return status;
}
