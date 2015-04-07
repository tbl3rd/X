#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <tmc/cpus.h>

#include "packets.h"
#include "process.h"
#include "route.h"
#include "tilera.h"
#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


#define ETHERNETHEADERSIZE (14)
#define MINIPHEADERSIZE (20)
#define UDPHEADERSIZE (8)
#define UDPPAYLOADOFFSET (UDPHEADERSIZE + MINIPHEADERSIZE + ETHERNETHEADERSIZE)
#define UDPPAYLOADSIZE (1316)
#define PACKETSIZE (UDPPAYLOADOFFSET + UDPPAYLOADSIZE)


// A count of packets sent per route to verify packet sequence.
//
static unsigned long long packetCount[R30TOTALCHANNELS];


// Return the port of arrival for the packet described at pi.  The port of
// arrival is the just 16-bit integer destination port in the UDP header.
// The byte fiddling works around potential alignment issues.
//
static int getUdpPortOfArrival(const PacketInfo *pi)
{
    static const int portOffset = 2;    // destination follows source port
    unsigned char *const portByte = pi->l3Data + pi->ipHeaderSize + portOffset;
    const int result = (portByte[0] << 8) | (portByte[1] << 0);
    INFO("__: getUdpPortOfArrival(%p) returns %d", pi, result);
    return result;
}


// Compute the IP (or UDP) checksum on the size bytes in buffer.  Ensure
// any pseudo header or zero checksum is in place before calling this.
//
static unsigned int ipCsum(const unsigned char *buffer, size_t size)
{
    INFO("__: ipCsum(%p, %zu)", buffer, size);
    unsigned long csum = 0;
    const unsigned char *const pEnd = buffer + size;
    const unsigned char *p = buffer;
    for (; p + 1 < pEnd; p += 2) csum += (p[0] << 8) | p[1];
    if (p < pEnd) csum += *(++p);
    while (csum >> 16) csum = (csum >> 16) + (0xffff & csum);
    const unsigned int result = 0xffff & ~csum;
    return result;
}


// Write size bytes from n into buffer.
//
static void fillBuffer(char *buffer, size_t size, unsigned long long n)
{
    INFO("__: fillBuffer(%p, %zu, %llu)", buffer, size, n);
    const unsigned char *const b = (unsigned char *)&n;
    for (int i = 0; i < size; ++i) buffer[i] = b[i % sizeof n];
}


// Compute a seed for NETIO_PKT_DO_EGRESS_CSUM() to compute a UDP checksum
// over the IPv4 pseudo-header such that the final checksum is valid for
// the actual header.  The seed is the uncomplemented 16-bit 1's complement
// checksum of the IPv4 addresses, a zero, the UDP protocol number (0x11)
// and the UDP packet size (including both the payload and UDP header).
//
static unsigned int udpCsumIpPseudoHeaderSeed(const Endpoint *dst,
                                              const Endpoint *src)
{
    static const unsigned int zeroProtocol = (0x00 << 8) | (0x11 << 0);
    static const unsigned int udpSize = UDPHEADERSIZE + UDPPAYLOADSIZE;
    unsigned long seed = zeroProtocol + udpSize +
        (0xffff & (src->ip[0] << 8) | (src->ip[1] << 0)) +
        (0xffff & (src->ip[2] << 8) | (src->ip[3] << 0)) +
        (0xffff & (dst->ip[0] << 8) | (dst->ip[1] << 0)) +
        (0xffff & (dst->ip[2] << 8) | (dst->ip[3] << 0));
    while (seed >> 16) seed = (seed >> 16) + (0xffff & seed);
    const unsigned int result = 0xffff & seed;
    return result;
}


#define DEBUG_NETIO_PKT_DO_EGRESS_CSUM NETIO_PKT_DO_EGRESS_CSUM
// #define DEBUG_NETIO_PKT_DO_EGRESS_CSUM(pkt, dBegin, dSize, cBegin, cSeed) \
//     do {info("__: NETIO_PKT_DO_EGRESS_CSUM " #pkt    " == %p", pkt);    \
//         info("__: NETIO_PKT_DO_EGRESS_CSUM " #dBegin " == %u", dBegin); \
//         info("__: NETIO_PKT_DO_EGRESS_CSUM " #dSize  " == %u", dSize);  \
//         info("__: NETIO_PKT_DO_EGRESS_CSUM " #cBegin " == %u", cBegin); \
//         info("__: NETIO_PKT_DO_EGRESS_CSUM " #cSeed  " == %u", cSeed);  \
//         NETIO_PKT_DO_EGRESS_CSUM(pkt, dBegin, dSize, cBegin, cSeed);    \
//     } while (0);

// Write an Ethernet packet from src to dst in pkt.  Fill out PACKETSIZE
// bytes with repetitions of the value n.  Tell NETIO to calculate both the
// IPv4 header checksum and the UDP packet checksum.  (Assume the EPP will
// manage the Ethernet frame check sequence CRC?)
//
static void buildPacket(netio_pkt_t *pkt, unsigned long long n,
                        const Endpoint *dst, const Endpoint *src)
{
    INFO("__: buildPacket(%p, %llu, %p, %p)", pkt, n, dst, src);
    static const unsigned char etherType[2]  = { 0x08, 0x00 };
    static const unsigned char ipVersion     = 0x4; // IPv4
    static const unsigned char ipIhl         = 0x5; // 4-byte words in header
    static const unsigned char ipDscpEcn     = 0;
    static const unsigned char ipIdent[2]    = { 0x00, 0x00 };
    static const unsigned char ipFlagFrag    = 0x40; // 010 | 0
    static const unsigned char ipFragOffset  = 0x00;
    static const unsigned char ipTimeToLive  = 0x3f;
    static const unsigned char ipProtocolUdp = 0x11;
    const unsigned char ipVersionIhl  = (ipVersion << 4) | (ipIhl << 0);
    unsigned char *const pBegin = NETIO_PKT_L2_DATA(pkt);
    const unsigned char *const pEnd = pBegin + NETIO_PKT_L2_LENGTH(pkt);
    unsigned char *p = pBegin;
    memcpy(p, dst->mac,  sizeof dst->mac);  p += sizeof dst->mac;
    memcpy(p, src->mac,  sizeof src->mac);  p += sizeof src->mac;
    memcpy(p, etherType, sizeof etherType); p += sizeof etherType;
    const unsigned char *const ipHeaderBegin = p;
    const unsigned int ipHeaderOffset = ipHeaderBegin - pBegin;
    const unsigned int ipTotalSize = pEnd - ipHeaderBegin;
    *p++ = ipVersionIhl;
    *p++ = ipDscpEcn;
    *p++ = 0xff & (ipTotalSize >> 8);
    *p++ = 0xff & (ipTotalSize >> 0);
    memcpy(p, ipIdent,   sizeof ipIdent);   p += sizeof ipIdent;
    *p++ = ipFlagFrag;
    *p++ = ipFragOffset;
    *p++ = ipTimeToLive;
    *p++ = ipProtocolUdp;
    const unsigned int ipHeaderCsumOffset = p - pBegin;
    const unsigned int ipHeaderCsumSeed = 0;
    *p++ = 0xff & (ipHeaderCsumSeed >> 8);
    *p++ = 0xff & (ipHeaderCsumSeed >> 0);
    memcpy(p, src->ip,   sizeof src->ip);   p += sizeof src->ip;
    memcpy(p, dst->ip,   sizeof dst->ip);   p += sizeof dst->ip;
    const unsigned int ipHeaderSize = p - ipHeaderBegin;
    const unsigned int udpOffset = p - pBegin;
    const unsigned int udpSize = pEnd - p;
    *p++ = 0xff & (src->port >> 8);
    *p++ = 0xff & (src->port >> 0);
    *p++ = 0xff & (dst->port >> 8);
    *p++ = 0xff & (dst->port >> 0);
    *p++ = 0xff & (udpSize >> 8);
    *p++ = 0xff & (udpSize >> 0);
    const unsigned int udpCsumOffset = p - pBegin;
    unsigned int udpCsumSeed = 0;
    *p++ = 0xff & (udpCsumSeed >> 8);
    *p++ = 0xff & (udpCsumSeed >> 0);
    fillBuffer(p, pEnd - p, n);
    udpCsumSeed = udpCsumIpPseudoHeaderSeed(dst, src);
    DEBUG_NETIO_PKT_DO_EGRESS_CSUM(pkt, // Checksum IPv4 header on send.
                                   ipHeaderOffset, ipHeaderSize,
                                   ipHeaderCsumOffset, ipHeaderCsumSeed);
    DEBUG_NETIO_PKT_DO_EGRESS_CSUM(pkt, // Checksum UDP packet on send.
                                   udpOffset, udpSize,
                                   udpCsumOffset, udpCsumSeed);
}


// Write a packet for route rt to t->queue.
//
static void packetSendOne(Thread *t, const Route *rt)
{
    INFO("%02d: packetSendOne(%p, %p)", t->index, t, rt);
    Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    netio_pkt_t pkt;
    netio_error_t err = netio_get_buffer(q, &pkt, PACKETSIZE, 1);
    if (err != NETIO_NO_ERROR) {
        error("%02d: netio_get_buffer(%p, %p, %d, 1) returned %d: %s",
              t->index, q, &pkt, PACKETSIZE, err, netio_strerror(err));
    }
    netio_populate_buffer(&pkt);
    NETIO_PKT_SET_L2_LENGTH(&pkt, PACKETSIZE);
    NETIO_PKT_SET_L2_HEADER_LENGTH(&pkt, ETHERNETHEADERSIZE);
    Endpoint dst = p->control;
    Endpoint src = p->forward;
    dst.port = src.port = rt->poa;
    buildPacket(&pkt, packetCount[rt->index], &dst, &src);
    // dumpPacket(&pkt, "./dump-tester.dat");
    err = NETIO_QUEUE_FULL;
    while (err == NETIO_QUEUE_FULL) err = netio_send_packet(q, &pkt);
    if (err == NETIO_NO_ERROR) {
        ++t->send[rt->index];
    } else {
        error("%02d: netio_send_packet(%p, %p) returned %d: %s",
              t->index, q, &pkt, err, netio_strerror(err));
    }
    SLEEP(1);
}


// Free the packet buffer at pkt from q for thread t.
//
static void freePacketBuffer(const Thread *t,
                             netio_queue_t *q, netio_pkt_t *pkt)
{
    const netio_error_t err = netio_free_buffer(q, pkt);
    if (err != NETIO_NO_ERROR) {
        error("%02d: netio_free_buffer(%p, %p) returned %d: %s",
              t->index, q, pkt, err, netio_strerror(err));
    }
}


// Read a packet containing n from t->queue, and write a new packet to
// t->queue containing n + 1.  Invalidate twice the size of the header
// to make sure some of the packet numbering is cached.
//
static void packetReceiveAndSend(Thread *t)
{
    // INFO("%02d: packetReceiveAndSend(%p)", t->index, t); // too much spew
    const Process *const p = t->process;
    netio_queue_t *const q = &t->queue;
    int packetSent = 0;
    netio_pkt_t pkt;
    const netio_error_t err = netio_get_packet(q, &pkt);
    if (err == NETIO_NO_ERROR) {
        const PacketInfo pi = parsePacket(p, &pkt);
        ++t->status[pi.status];
        INFO("%02d: packetReceiveAndSend(%p) got packet on %d",
             t->index, t, pi.poa);
        if (pi.isUdpForMe) {
            const Route rt = routeFromPortOfArrival(pi.poa);
            if (rt.index < 0) {
                error("%02d: packetReceiveAndSend(%p) with poa %d index %d",
                      t->index, t, pi.poa, rt.index);
                assert(rt.index >= 0);
            }
            ++t->recv[rt.index];
            netio_pkt_inv(pi.l2Data, 2 * pi.allHeadersSize);
            const unsigned char *const pN = pi.l2Data + pi.allHeadersSize;
            INFO("%02d: pN == %p, pi.l2Data == %p, pi.allHeadersSize == %d",
                 t->index, pN, pi.l2Data, pi.allHeadersSize);
            unsigned long long n = 0;
            for (int i = sizeof n; i-- > 0;) n = (n << 8) | pN[i];
            INFO("%02d: packetReceiveAndSend(%p) finds n %llu count %llu",
                 t->index, t, n, packetCount[rt.index]);
            if (n != packetCount[rt.index]) ++t->drop[rt.index];
            packetCount[rt.index] = n;
            ++packetCount[rt.index];
            freePacketBuffer(t, q, &pkt);
            if (n < p->packetCount) packetSendOne(t, &rt);
        } else {
            // info("%02d: packetReceiveAndSend(%p) forwards to TAP %d: %s",
            //      t->index, t, pi.status, netio_strerror(pi.status));
            ++t->tap;
            const int wCount = write(p->tap, pi.l2Data, pi.l2Length);
            if (wCount < 0) {
                error("%02d: write(%d, %p, %zu) returned %d with errno %d: %s",
                      t->index, p->tap, pi.l2Data, pi.l2Length, wCount,
                      errno, strerror(errno));
            }
            freePacketBuffer(t, q, &pkt);
        }
    } else if  (err == NETIO_NOPKT) {
        //
        // Return and get again after checking for an alert.
        //
    } else {
        error("%02d: netio_get_packet(%p, %p) returned %d: %s",
              t->index, q, &pkt, err, netio_strerror(err));
    }
}


void packetsPrimePipeline(struct Thread *t)
{
    INFO("%02d: packetsPrimePipeline(%p)", t->index, t);
    Process *const p = t->process;
    for (int n = 0; n < p->routeCount; ++n) {
        const Route rt = routeFromPortOfArrival(PORTOFFSET + n);
        if (rt.open) {
            packetSendOne(t, &rt);
            SLEEP(1);
        }
    }
}


void *packetsStart(void *v)
{
    Thread *const t = (Thread *)v;
    Process *const p = t->process;
    INFO("%02d: packetsStart(%p)", t->index, t);
    const int fail = tmc_cpus_set_my_cpu(t->cpu);
    if (fail) {
        error("%02d: tmc_cpus_set_my_cpu(%d) returned %d",
              t->index, t->cpu, fail);
    }
    registerQueueReadWrite(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    while (!t->alert) packetReceiveAndSend(t);
    INFO("%02d: packetsStart(%p) alerted", t->index, t);
    unregisterQueue(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    return v;
}
