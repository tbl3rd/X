#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>                 // Must precede the if.h files.

#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include <tmc/cpus.h>

#include "process.h"
#include "tap.h"
#include "tilera.h"
#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)

// The name of the Linux TAP device node.
//
#define TAPDEVICE "/dev/net/tun"

// The maximum size of one of our Ethernet packets.
//
#define PACKETSIZE (8192)


// Configure the TAP device for p.
//
void tapConfigure(Process *p)
{
    INFO("__: tapConfigure(%p)", p);
    p->tap = open(TAPDEVICE, O_RDWR);
    if (p->tap < 0) {
        error("__: open(%s, O_RDWR) returned %d with errno %d: %s",
              TAPDEVICE, p->tap, errno, strerror(errno));
    }
    struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI };
    int status = ioctl(p->tap, TUNSETIFF, &ifr);
    if (status < 0) {
        error("__: ioctl(%d, TUNSETIFF, %p) for %s returned %d "
              "with errno %d: %s",
              p->tap, &ifr, TAPDEVICE, status, strerror(errno));
    }
    INFO("__: Opened TAP: %s", ifr.ifr_name);
    static const char ifconfig[] = "/sbin/ifconfig";
    const unsigned char *const m = p->forward.mac;
    systemCommand("%s %s hw ether " MACFMT, ifconfig, ifr.ifr_name,
                  m[0], m[1], m[2], m[3], m[4], m[5]);
    const unsigned char *const ip = p->forward.ip;
    systemCommand("%s %s inet " IPFMT,
                  ifconfig, ifr.ifr_name, ip[0], ip[1], ip[2], ip[3]);
    systemCommand("%s %s netmask 255.255.0.0",
                  ifconfig, ifr.ifr_name);
    INFO("__: TAP configured: %s", ifr.ifr_name);
}


// Forward packets read from tap file descriptor to NETIO q on behalf of
// the program named av0.
//
static void tapToQueue(Thread *t, int tap, netio_queue_t *q)
{
    INFO("%02d: tapToQueue(%p, %d, %p)", t->index, t, tap, q);
    unsigned char buffer[PACKETSIZE];
    const int rSize = read(tap, buffer, sizeof buffer);
    INFO("%02d: TAP read(%d, %p, %zu) returned %d",
         t->index, tap, buffer, sizeof buffer, rSize);
    if (rSize == 0) {
        INFO("%02d: read(%d, %p, %zd) returned %zd for EOF",
             t->index, tap, buffer, sizeof buffer, rSize);
        INFO("%02d: tapToQueue(%p, %d, %q) returns on EOF",
             t->index, t, tap, q);
        t->alert = 1;
        close(tap);
    } else if (rSize < 0 || rSize > sizeof buffer) {
        error("%02d: TAP read(%d, %p, %zu) returned %d with errno %d: %s",
              t->index, tap, buffer, sizeof buffer, rSize,
              errno, strerror(errno));
    } else {
        ++t->recv[0];
        netio_pkt_t pkt;
        netio_get_buffer(q, &pkt, rSize, 1);
        netio_populate_buffer(&pkt);
        NETIO_PKT_SET_L2_LENGTH(&pkt, rSize);
        unsigned char *const payload = NETIO_PKT_L2_DATA(&pkt);
        memcpy(payload, buffer, rSize);
        netio_pkt_flush(&pkt, rSize);
        netio_pkt_fence();
        netio_error_t err = NETIO_QUEUE_FULL;
        while (err == NETIO_QUEUE_FULL) err = netio_send_packet(q, &pkt);
        if (err == NETIO_NO_ERROR) {
            ++t->send[0];
        } else {
            ++t->drop[0];
            error("%02d: TAP netio_send_packet(%p, %p) returned %d: %s",
                  t->index, q, &pkt, err, netio_strerror(err));
        }
    }
}


// Forward non-UDP packets to TAP device on p->tap.
//
void *tapStart(void *v)
{
    Thread *const t = (Thread *)v;
    INFO("%02d: tapStart(%p)", t->index, t);
    Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    const int tap = p->tap;
    const int fail = tmc_cpus_set_my_cpu(t->cpu);
    if (fail) {
        error("%02d: tmc_cpus_set_my_cpu(%d) returned %d",
              t->index, t->cpu, fail);
    }
    registerQueueWrite(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    while (!t->alert) tapToQueue(t, tap, q);
    INFO("%02d: TAP tapStart(%p) alerted", t->index, t);
    unregisterQueue(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    return t;
}
