#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "process.h"
#include "route.h"
#include "tilera.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Register t with NETIO.  Register for writes if writing and reads if
// reading.
//
// If registration fails because the Ethernet link is not up, then retry
// every second until the link is up or another error occurs.
//
// NETIO_NOREQUIRE_LINK_UP means don't wait for the link to come up and
// don't return the NETIO_LINK_DOWN error.  The Ethernet interface is down
// until the netio_input_initialize() call in initializeNetio() returns.
// NETIO_AUTO_LINK_UP is the default, but that returns NETIO_LINK_DOWN when
// main() registers its queue without IO enabled.
//
static void registerQueue(Thread *t, int writing, int reading)
{
    INFO("%02d: registerQueue(%p, %d, %d)", t->index, t, writing, reading);
    Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    const int w = writing? NETIO_XMIT: NETIO_NO_XMIT;
    const int r = reading? NETIO_RECV: NETIO_NO_RECV;
    const unsigned int flags =
        NETIO_XMIT | NETIO_TAG_NONE | NETIO_NOREQUIRE_LINK_UP;
    netio_input_config_t config = {
        .flags =  w | r | flags,
        .num_receive_packets = NETIO_MAX_RECEIVE_PKTS,
        .interface = p->interface,
        .queue_id = t->index
    };
    netio_error_t err = NETIO_LINK_DOWN;
    while (err == NETIO_LINK_DOWN) {
        const netio_error_t err = netio_input_register(&config, q);
        if (err == NETIO_LINK_DOWN) {
            info("%02d: netio_input_register(%p, %p) for interface %s "
                 "on CPU %2d returned %d: %s", t->index, &config, q,
                 config.interface, t->cpu, err, netio_strerror(err));
        } else if (err == NETIO_NO_ERROR) {
            INFO("%02d: register queue for interface %s on CPU %2d",
                 t->index, config.interface, t->cpu);
            break;
        } else {
            error("%02d: netio_input_register(%p, %p) for interface %s "
                  "on CPU %2d returned %d: %s", t->index, &config, q,
                  config.interface, t->cpu, err, netio_strerror(err));
            assert(0);
        }
    }
}
void registerQueueRead(struct Thread *t)      { registerQueue(t, 0, 1); }
void registerQueueReadWrite(struct Thread *t) { registerQueue(t, 1, 1); }
void registerQueueWrite(struct Thread *t)     { registerQueue(t, 1, 0); }
void registerQueueStatsOnly(struct Thread *t) { registerQueue(t, 0, 0); }


void unregisterQueue(Thread *t)
{
    INFO("%02d: unregisterQueue(%p)", t->index, t);
    Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    const netio_error_t err = netio_input_unregister(q);
    if (err == NETIO_NO_ERROR) {
        INFO("%02d: unregister queue for CPU %2d", t->index, t->cpu);
    } else {
        error("%02d: netio_input_unregister(%p) on CPU %2d "
              "returned %d: %s", t->index, q, t->cpu,
              err, netio_strerror(err));
    }
}


// Configure pause frames on the interface identified by q.  Suspend output
// on the interface when it receives a pause frame.  If send, also send
// pause frames back to a packet source when the IO shim packet FIFOs are
// nearly full to avoid buffer overflow.
//
// A pause is the time it takes to send or receive 512 bits.
//
static void configurePauseFrames(netio_queue_t *q, int send)
{
    INFO("__: configurePauseFrames(%p, %d)", q, send);
    int one = 1;
    const int size = netio_set(q, NETIO_PARAM, NETIO_PARAM_PAUSE_IN,
                               (void *)&one, sizeof one);
    if (size != sizeof one) error("__: Failed to set up IN pauses.");
    if (send) {
        int pauses = 1;                 // in 512 bit time
        const int size = netio_set(q, NETIO_PARAM, NETIO_PARAM_PAUSE_OUT,
                                   (void *)&pauses, sizeof pauses);
        if (size != sizeof pauses) error("__: Failed to set up OUT pauses.");
    }
}


// Initialize Tilera's NETIO for the routing process p.  Set up to register
// one queue per tile, with tile 0 listening for commands, tile 1 writing
// the TAP device, and tiles 2 and above routing UDP packets while using
// the CPU index as the queue ID.  So here is the strategy ...
//
// Create a group and map of buckets on a single interface (or IPP: Input
// Packet Processor) for all the UDP forwarding queues.  Each IPP has
// NETIO_NUM_BUCKETS at its disposal where
//
//   NETIO_NUM_BUCKETS == 1 << NETIO_LOG2_NUM_BUCKETS
//
// It looks like NETIO_LOG2_NUM_BUCKETS is 10 these days, so we can use up
// to 0x400 (1024) buckets.  Choose 0x200 for the forwarding queues to
// leave some buckets for the command and TAP IO functions on threads 0 and
// 1, and to cover the threadCount - 2 forwarding queues many times over.
//
// Group packets into buckets by hashing on Ethernet (L2), IP (L3) and UDP
// (L4) headers.  Specify all encapsulation layers so even ARP (L2) packets
// are not lost.  The hash always uses the highest layer available.
//
// Hash to 512 buckets numbered 0 through 0x1ff.  (Mask the header hash with
// bucket_mask adding the result to bucket_base.)
//
// Map bucket N to queue b2q[N] such that the (threadCount - 2) queue IDs
// (aka thread indexes) starting at 2 occur with about equal probability.
//
// Any queue will do here because they are all on the same interface (IPP),
// so just use thread[0].queue.  And why not squirrel away the MAC address
// of the interface (identified by the queue) here?
//
// FYI: The netio_input_UNinitialize() call is not yet implemented.
//
#define LOG2 (NETIO_LOG2_NUM_BUCKETS - 1)
#define COUNT (1 << LOG2)
#define MASK (COUNT - 1)
void initializeNetio(struct Process *p)
{
    INFO("__: initializeNetio(%p)", p);
    Thread *const t = p->thread + 0;
    netio_queue_t *const q = &t->queue;
    static netio_group_t group = {
        .bits.__balance_on_l4 = 1,      // Hash on port numbers.
        // .bits.__balance_on_l3 = 1,      // Hash on IP addresses.
        // .bits.__balance_on_l2 = 1,      // Hash on MAC addresses
        .bits.__bucket_base   = 0,
        .bits.__bucket_mask   = MASK
    };
    INFO("__: %u (0x%x) buckets with mask 0x%x", COUNT, COUNT, MASK);
    netio_error_t err = NETIO_NO_ERROR;
    static netio_bucket_t b2q[COUNT];
    for (int n = 0; n < COUNT; ++n) {
        b2q[n] = p->netioThreadIndex + n % p->netioThreadCount;
    }
    err = netio_input_bucket_configure(q, group.bits.__bucket_base, b2q, COUNT);
    if (err != NETIO_NO_ERROR) {
        error("__: netio_input_bucket_configure(%p, %d, %p, 1 << %d) "
              "returned %d: %s",
              q, group.bits.__bucket_base, b2q, LOG2,
              err, netio_strerror(err));
    }
    err = netio_input_group_configure(q, 0, &group, 1);
    if (err != NETIO_NO_ERROR) {
        error("__: netio_input_group_configure(%p, 0, %p, 1) returned %d: %s",
              q, &group, err, netio_strerror(err));
    }
    configurePauseFrames(q, 1);
    err = netio_input_initialize(q);
    if (err != NETIO_NO_ERROR) {
        error("__: netio_input_initialize(%p) returned %d: %s",
              q, err, netio_strerror(err));
    }
    const int size = netio_get(q, NETIO_PARAM, NETIO_PARAM_MAC,
                               p->forward.mac, sizeof p->forward.mac);
    if (size != sizeof p->forward.mac) {
        error("%02d: netio_get(%p, NETIO_PARAM, NETIO_PARAM_MAC, %p, %d) "
              "returned %d: %s",
              t->index, q, p->forward.mac, sizeof p->forward.mac,
              size, netio_strerror(size));
    }
}
#undef MASK
#undef COUNT
#undef LOG2


static void dumpPacketInfo(const PacketInfo *pi)
{
    info("__: ((PacketInfo *)%p)->isUdpForMe     == %d",  pi, pi->isUdpForMe);
    info("__: ((PacketInfo *)%p)->poa            == %d",  pi, pi->poa);
    info("__: ((PacketInfo *)%p)->pkt            == %p",  pi, pi->pkt);
    info("__: ((PacketInfo *)%p)->md             == %p",  pi, pi->md);
    info("__: ((PacketInfo *)%p)->status         == %lu", pi, pi->status);
    info("__: ((PacketInfo *)%p)->l2Data         == %p",  pi, pi->l2Data);
    info("__: ((PacketInfo *)%p)->l3Data         == %p",  pi, pi->l3Data);
    info("__: ((PacketInfo *)%p)->l2Length       == %u",  pi, pi->l2Length);
    info("__: ((PacketInfo *)%p)->ipHeaderSize   == %u",  pi, pi->ipHeaderSize);
    info("__: ((PacketInfo *)%p)->allHeadersSize == %u",
         pi, pi->allHeadersSize);
}


// Return a PacketInfo describing the NETIO packet at pkt for p.
//
// Invalidate the packet's standard NETIO metadata first.  Then invalidate
// the minimal range of the combined Ethernet, IP, and UDP headers.  If
// result.isUdpForMe, calculate the actual size of the IP header and extend
// the invalidation range.  Otherwise, invalidate the entire Ethernet
// packet so all the data is cached for forwarding to the TAP device.
//
// (L2 is Ethernet (L3 is IP (and L4 is kind of UDP))).
//
// The packet isUdpForMe if its protocol field (9 bytes into the IP header)
// is 0x11, the IP version (first 4 bits of the IP header) is 4, the size
// of the combined IP and UDP headers is greater than the minimum length 28
// (20 bytes of IP + 8 bytes of UDP), and the MAC address (first 6 bytes of
// the Ethernet header) matches the forwarding interface's MAC address
// (p->forward.mac).
//
// The actual IP header length is in the low 4 bits of the first byte of
// the IP header in units of 4-byte words.
//
const PacketInfo parsePacket(const Process *p, netio_pkt_t *pkt)
{
    INFO("__: parsePacket(%p, %p)", p, pkt);
    static const unsigned char protocolByte = 9;    // offset to IANA protocol
    static const unsigned char protocolUdp = 0x11;  // IANA protocol number
    static const int ipVersionByte = 0;             // offset to IP version
    static const unsigned char ipVersionMask = 0xf0;
    static const int ipVersionShift = 4;            // high 4 bits of byte 0
    static const unsigned char ipV4Version = 4;     // of course
    static const unsigned char ipHeaderSizeMask = 0x0f;
    static const int ipHeaderSizeShift = 0;         // low 4 bits of byte 0
    static const int minIpHeaderLength = 20;        // minimum IP header
    static const int udpHeaderLength = 8;           // UDP header length
    static const int portOffset = 2;                // dst follows src port
    const int minUdpLength = minIpHeaderLength + udpHeaderLength;
    netio_pkt_metadata_t *const md = NETIO_PKT_METADATA(pkt);
    NETIO_PKT_INV_METADATA_M(md, pkt);
    PacketInfo result = {
        .isUdpForMe = 0, .pkt = pkt, .md = md,
        .status = NETIO_PKT_STATUS_M(md, pkt),
        .l2Data = NETIO_PKT_L2_DATA_M(md, pkt),
        .l3Data = NETIO_PKT_L3_DATA_M(md, pkt),
        .l2Length = NETIO_PKT_L2_LENGTH_M(md, pkt),
        .l3Length = NETIO_PKT_L3_LENGTH_M(md, pkt)
    };
    const unsigned int ethernetHeaderLength = result.l3Data - result.l2Data;
    const unsigned int minHeadersLength = ethernetHeaderLength + minUdpLength;
    result.ipHeaderSize = minIpHeaderLength;
    result.allHeadersSize = ethernetHeaderLength;
    netio_pkt_inv(result.l2Data, minHeadersLength); // Read minimal headers.
    unsigned char *const portByte =
        result.l3Data + result.ipHeaderSize + portOffset;
    result.poa = (portByte[0] << 8) | (portByte[1] << 0);
    // info("__: parsePacket() minUdpLength == %d", minUdpLength);
    // info("__: parsePacket() protocolUdp == %02x",
    //      result.l3Data[protocolByte]);
    // info("__: parsePacket() ipV4Version == %02x",
    //      ((ipVersionMask & result.l3Data[ipVersionByte])
    //       >> ipVersionShift));
    // const unsigned char *const m = result.l2Data;
    // info("__: parsePacket() mac == " MACFMT,
    //      m[0], m[1], m[2], m[3], m[4], m[5]);
    result.isUdpForMe =
        (result.l3Length > minUdpLength) &&
        (result.l3Data[protocolByte] == protocolUdp) &&
        (((ipVersionMask & result.l3Data[ipVersionByte]) >> ipVersionShift)
         == ipV4Version) &&
        (memcmp(result.l2Data, p->forward.mac, sizeof p->forward.mac) == 0);
    if (result.isUdpForMe) {
        const unsigned int ipHeaderSizeInWords =
            (ipHeaderSizeMask & result.l3Data[ipVersionByte])
            >> ipHeaderSizeShift;
        result.ipHeaderSize = 4 * ipHeaderSizeInWords;
        result.allHeadersSize = minHeadersLength;
        if (result.ipHeaderSize > minIpHeaderLength) {
            result.allHeadersSize =
                ethernetHeaderLength + result.ipHeaderSize + udpHeaderLength;
        }
        unsigned char *const portByte =
            result.l3Data + result.ipHeaderSize + portOffset;
        result.poa = (portByte[0] << 8) | (portByte[1] << 0);
        netio_pkt_inv(result.l2Data, result.allHeadersSize);
    } else {
        netio_pkt_inv(result.l2Data, result.l2Length); // Read entire packet.
    }
    // dumpPacketInfo(&result);
    return result;
}


static void showNonNetioThread(const Thread *t, const char *name)
{
    unsigned long long drop = 0;
    unsigned long long recv = 0;
    unsigned long long send = 0;
    int activeRouteCount = 0;
    for (int n = 0; n < R30TOTALCHANNELS; ++n) {
        if (t->drop[n] || t->recv[n] || t->send[n]) {
            drop += t->drop[n];
            recv += t->recv[n];
            send += t->send[n];
            ++activeRouteCount;
        }
    }
    if (activeRouteCount) {
        show("The %s thread %d on CPU %d showed activity on %d routes",
             name, t->index, t->cpu, activeRouteCount);
        show("%s: packet counts: %5llu drop %5llu recv %5llu send",
             name, drop, recv, send);
    } else {
        show("The %s thread %d on CPU %d showed no packet activity.",
             name, t->index, t->cpu);
    }
    if (t->tap) {
        show("The %s thread %2d on CPU %2d forwarded %5llu packets to TAP.",
             name, t->index, t->cpu, t->tap);
    }
}


static void showNetioPacketStatus(const Process *p)
{
    static const size_t statusCount =
        sizeof p->thread->status / sizeof p->thread->status[0];
    for (int m = p->netioThreadIndex; m < p->threadCount; ++m) {
        const Thread *const t = p->thread + m;
        for (int n = 0; n < statusCount; ++n) {
            if (t->status[n]) {
                show("Thread %2d: %5llu ok"
                     " %5llu under %5llu over %5llu bad packets",
                     t->index, t->status[NETIO_PKT_STATUS_OK],
                     t->status[NETIO_PKT_STATUS_UNDERSIZE],
                     t->status[NETIO_PKT_STATUS_OVERSIZE],
                     t->status[NETIO_PKT_STATUS_BAD]);
                break;
            }
        }
    }
}


static void showNetioThreads(const Process *p)
{
    int routesPerThread[MAXCPUCOUNT] = {};
    int threadsPerRoute[R30TOTALCHANNELS] = {};
    const Thread *threadOnRoute[R30TOTALCHANNELS][MAXCPUCOUNT] = {};
    unsigned long long dropPerRoute[R30TOTALCHANNELS] = {};
    unsigned long long recvPerRoute[R30TOTALCHANNELS] = {};
    unsigned long long sentPerRoute[R30TOTALCHANNELS] = {};
    for (int m = p->netioThreadIndex; m < p->threadCount; ++m) {
        const Thread *const t = p->thread + m;
        for (int n = 0; n < R30TOTALCHANNELS; ++n) {
            dropPerRoute[n] += t->drop[n];
            recvPerRoute[n] += t->recv[n];
            sentPerRoute[n] += t->send[n];
            if (t->drop[n] || t->recv[n] || t->send[n]) {
                ++routesPerThread[m];
                threadOnRoute[n][threadsPerRoute[n]] = t;
                ++threadsPerRoute[n];
            }
        }
    }
    for (int n = 0; n < R30TOTALCHANNELS; ++n) {
        const int tc = threadsPerRoute[n];
        if (tc) {
            const int poa = PORTOFFSET + n;
            const Route rt = routeFromPortOfArrival(poa);
            const unsigned char *i = rt.dst.ip;
            const unsigned char *m = rt.dst.mac;
            show("Route %d: %2d threads to " IPFMT ":%d (" MACFMT ")",
                 poa, tc, i[0], i[1], i[2], i[3], rt.dst.port,
                 m[0], m[1], m[2], m[3], m[4], m[5]);
            char threadList[999];
            char *pTl = threadList;
            const char *const pTlEnd = threadList + sizeof threadList;
            for (int m = 0; m < tc; ++m) {
                pTl += snprintf(pTl, pTlEnd - pTl, " %02d",
                                threadOnRoute[n][m]->index);
                assert(pTl < pTlEnd);
            }
            show("Route %d had %2d threads:%s", poa, tc, threadList);
            show("Route %d had packet counts: "
                 "%5llu drop %5llu recv %5llu send", poa,
                 dropPerRoute[n], recvPerRoute[n], sentPerRoute[n]);
        }
    }
    for (int m = p->netioThreadIndex; m < p->threadCount; ++m) {
        const Thread *const t = p->thread + m;
        if (routesPerThread[m]) {
            show("Thread %2d on CPU %2d had %d routes",
                 t->index, t->cpu, routesPerThread[m]);
            for (int n = 0; n < R30TOTALCHANNELS; ++n) {
                const int poa = PORTOFFSET + n;
                if (t->drop[n] || t->recv[n] || t->send[n]) {
                    show("Thread %2d route %d: "
                         "%5llu drop %5llu recv %5llu send", t->index, poa,
                         t->drop[n], t->recv[n], t->send[n]);
                }
            }
        }
        if (t->tap) {
            show("Thread %2d on CPU %2d forwarded %5llu packets to TAP.",
                 t->index, t->cpu, t->tap);
        }
    }
}


static void showNetioStatistics(Process *p)
{
    Thread *const t = p->thread + 0;
    netio_queue_t *const q = &t->queue;
    unsigned long shimOverflowCounter = 0;
    int size = netio_get(q, NETIO_PARAM, NETIO_PARAM_OVERFLOW,
                         &shimOverflowCounter, sizeof shimOverflowCounter);
    if (size != sizeof shimOverflowCounter) {
        error("__: netio_get(NETIO_PARAM_OVERFLOW) returned %d not %d",
              size, sizeof shimOverflowCounter);
    }
    const unsigned long shimOverflowDropped =
        0xffff & (shimOverflowCounter >> 0);
    const unsigned long shimOverflowTruncated =
        0xffff & (shimOverflowCounter >> 16);
    show("IO shim dropped %lu packets and truncated %lu packets",
         shimOverflowDropped, shimOverflowTruncated);
    netio_stat_t netioStatistics = {};
    size = netio_get(q, NETIO_PARAM, NETIO_PARAM_STAT,
                     &netioStatistics, sizeof netioStatistics);
    if (size != sizeof netioStatistics) {
        error("__: netio_get(NETIO_PARAM_STAT) returned %d not %d",
              size, sizeof netioStatistics);
    }
    show("IPP received %lu packets and dropped %lu packets",
         netioStatistics.packets_received, netioStatistics.packets_dropped);
    if (netioStatistics.drops_no_worker) {
        show("IPP dropped %lu packets because no worker was available",
             netioStatistics.drops_no_worker);
    }
    if (netioStatistics.drops_no_smallbuf) {
        show("IPP dropped %lu packets because there was no small buffer",
             netioStatistics.drops_no_smallbuf);
    }
    if (netioStatistics.drops_no_largebuf) {
        show("IPP dropped %lu packets because there was no large buffer",
             netioStatistics.drops_no_largebuf);
    }
    if (netioStatistics.drops_no_jumbobuf) {
        show("IPP dropped %lu packets because there was no jumbo buffer",
             netioStatistics.drops_no_jumbobuf);
    }
}


void showCounters(Process *p)
{
    INFO("__: showCounters(%p)", p);
    int routesPerThread[MAXCPUCOUNT] = {};
    int threadsPerRoute[R30TOTALCHANNELS] = {};
    show("Process with %2d threads saw %d route commands",
         p->threadCount, p->routeCount);
    show("Process has %2d NETIO threads starting at thread %d",
         p->netioThreadCount, p->netioThreadIndex);
    showNonNetioThread(p->thread + 0, "main()");
    showNonNetioThread(p->thread + 1, "TAPdev");
    showNetioThreads(p);
    showNetioPacketStatus(p);
    showNetioStatistics(p);
}


// Dump the packet at pkt into file.
//
void dumpPacket(netio_pkt_t *pkt, const char *file)
{
    netio_pkt_minimal_metadata_t *const md = NETIO_PKT_MINIMAL_METADATA(pkt);
    const unsigned char *const l2Data = NETIO_PKT_L2_DATA_MM(md, pkt);
    const unsigned int l2Length = NETIO_PKT_L2_LENGTH_MM(md, pkt);
    const int fd = open(file, O_APPEND | O_CREAT | O_WRONLY, 0666);
    INFO("__: dumpPacket(%p, %s) call write(%d, %p, %d)",
         pkt, file, fd, l2Data, l2Length);
    const int size = write(fd, l2Data, l2Length);
    if (size != l2Length) {
        error("__: dumpPacket(%p) write(%d, %p, %d) returned %d "
              "with errno %d: %s", pkt, fd, l2Data, l2Length, size,
              errno, strerror(errno));
    }
    close(fd);
}
