#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <tmc/cpus.h>

#include "forward.h"
#include "process.h"
#include "route.h"
#include "tilera.h"
#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Update the packet described by pi that arrived on port poa to be
// forwarded on the route at r.
//
// Compute the outgoing IP and UDP checksums from the incoming checksums
// and route, then write them into the packet.  The UDP checksum is
// optional, and can never be 0, so leave it alone if the incoming
// checksum was 0.
//
// According to RFC 1071 (and Wikipedia) ...
//
//     The checksum field is the 16-bit one's complement of the one's
//     complement sum of all 16-bit words in the header.  For purposes of
//     computing the checksum, the value of the checksum field is zero.
//
//   For example, consider Hex 4500003044224000800600008c7c19acae241e2b
//   (20 bytes IP header):
//
//     Step 1. 4500 + 0030 + 4422 + 4000 + 8006
//           + 0000 + 8c7c + 19ac + ae24 + 1e2b = 2BBCF (16-bit sum)
//
//     Step 2. 0002 + BBCF = BBD1 (1's complement 16-bit sum)
//
//     Step 3. ~BBD1 = 0100010000101110 = 442E
//            (1's complement of 1's complement 16-bit sum)
//
// So copy the old checksums into pi, then update them according to the
// algorithm described in RFC 1624.
//
//     newCsum = ~ ( ~oldCsum + ~oldWord + newWord + ... )
//
// Where newCsum is the new outgoing UDP or IP checksum, oldCsum is the old
// incoming checksum, oldWord is the old incoming 16-bit value, and newWord
// is the new 16-bit value to replace oldWord, and so on.  ~ is the bitwise
// complement operator, and + is 1's complement addition with carry.
//
// Compute new checksums from the old checksums by embedding the old 16-bit
// checksums in a 32-bit accumulator and computing deltas based on the
// incoming header fields in the packet and the outgoing fields from route
// r.  Assume the sum doesn't carry out of the accumulator, and defer the
// carries to the end.
//
// First shift the incoming 16-bit checksums into ipCsum and udpCsum.
//   The IP checksum is the 16 bits located 10 bytes into the IP header.
//   The UDP checksum is the 16 bits located 6 bytes into the UDP header.
//   1's complement the checksums and mask back down to 16 bits.
//
// Subtract the incoming destination port (the port of arrival) from the
// UDP checksum, then add in the new destination port from the route r.
// Always "subtract" by adding the 1's complement over 16 bits.
//
// Locate the incoming 32-bit IPv4 address 16 bytes into the IP header.
//
// Shift the incoming IPv4 destination address into two 16-bit values
// representing the high 2 bytes and the low 2 bytes of the address.
//
// Subtract the 16-bit values of the incoming IPv4 destination address from
// both the UDP and IP checksums.
//
// Shift the new outgoing IPv4 destination address from r into two 16-bit
// values representing the high and low 2 bytes of the address.
//
// Add the 16-bit values of the outgoing IPv4 destination address from r
// to both the UDP and IP checksums.
//
// Sum the high 2 bytes and low 2 bytes of each checksum, 1's complement
// the result and mask it back down to 16 bits.  (-What could go wrong?-)
//
static void updateUdpPacket(const PacketInfo *pi, const Route *rt, int poa)
{
    INFO("__: updateUdpPacket(%p, %p, %d)", pi, rt, poa);
    static const int ipCsumOffset = 10; // offset to IP header checksum
    static const int ipAddrOffset = 16; // offset to destination IP
    static const int macOffset = 0;     // offset to destination MAC
    static const int portOffset = 2;    // destination follows source port
    static const int udpCsumOffset = 6; // offset to UDP checksum
    unsigned char *const portByte = pi->l3Data + pi->ipHeaderSize + portOffset;
    unsigned char *const ipAddrByte  = pi->l3Data + ipAddrOffset;
    unsigned char *const ipCsumByte  = pi->l3Data + ipCsumOffset;
    unsigned char *const l4Data      = pi->l3Data + pi->ipHeaderSize;
    unsigned char *const udpCsumByte = l4Data + udpCsumOffset;
    unsigned int ipCsum =
        0xffff & ~  ((ipCsumByte[0] << 8) |  (ipCsumByte[1] << 0));
    unsigned int udpCsum =
        0xffff & ~ ((udpCsumByte[0] << 8) | (udpCsumByte[1] << 0));
    const int useUdpCsum = udpCsum != 0xffff;
    unsigned int ipHi2Bytes = (ipAddrByte[0] << 8) | (ipAddrByte[1] << 0);
    unsigned int ipLo2Bytes = (ipAddrByte[2] << 8) | (ipAddrByte[3] << 0);
    udpCsum += 0xffff & ~poa;        // subtract old destination port
    udpCsum += rt->dst.port;         // add new destination port
    udpCsum += 0xffff & ~ipHi2Bytes; // subtract old destination IP address
    udpCsum += 0xffff & ~ipLo2Bytes;
    ipCsum  += 0xffff & ~ipHi2Bytes;
    ipCsum  += 0xffff & ~ipLo2Bytes;
    ipHi2Bytes = (rt->dst.ip[0] << 8) | (rt->dst.ip[1] << 0);
    ipLo2Bytes = (rt->dst.ip[2] << 8) | (rt->dst.ip[3] << 0);
    udpCsum += ipHi2Bytes;              // add new destination IP address
    udpCsum += ipLo2Bytes;
    ipCsum  += ipHi2Bytes;
    ipCsum  += ipLo2Bytes;
    udpCsum = 0xffff & ~ ((udpCsum & 0xffff) + (udpCsum >> 16));
    ipCsum  = 0xffff & ~ ((ipCsum  & 0xffff) + (ipCsum  >> 16));
    portByte[0] = (rt->dst.port >> 8) & 0xff; // write new port number
    portByte[1] = (rt->dst.port >> 0) & 0xff; // then write new addresses
    memcpy(pi->l3Data + ipAddrOffset, rt->dst.ip,  sizeof rt->dst.ip);
    memcpy(pi->l2Data + macOffset,    rt->dst.mac, sizeof rt->dst.mac);
    if (useUdpCsum) {                   // write UDP checksum
        udpCsumByte[0] = (0xff00 & udpCsum) >> 8;
        udpCsumByte[1] = (0x00ff & udpCsum) >> 0;
    }
    ipCsumByte[0] = (0xff00 & ipCsum) >> 8; // write IP checksum
    ipCsumByte[1] = (0x00ff & ipCsum) >> 0;
}


// Send the NETIO packet described by pi on t->queue or drop it.
// Maintain the per-route packet counters here.
// Return 1 if the packet buffer must be freed with
// netio_free_buffer(&t->queue, pkt).  Otherwise return 0.
//
static int forwardPacketOnQueueOrDrop(Thread *t, const PacketInfo *pi)
{
    INFO("%02d: forwardPacketOnQueueOrDrop(%p, %p) with poa %d",
         t->index, t, pi, pi->poa);
    const Route const rt = routeFromPortOfArrival(pi->poa);
    if (rt.index < 0) {
        error("%02d: forwardPacketOnQueueOrDrop(%p, %p) with poa %d index %d",
              t->index, t, pi, pi->poa, rt.index);
        assert(rt.index >= 0);
    }
    ++t->recv[rt.index];
    if (pi->status == NETIO_PKT_STATUS_OK) {
        INFO("%02d: forwardPacketOnQueueOrDrop(%p, %p) poa ==  %d",
             t->index, t, pi, pi->poa);
        if (rt.open) {
            netio_populate_buffer(pi->pkt);
            updateUdpPacket(pi, &rt, pi->poa);
            // dumpPacket(pi->pkt, "./dump-switch.dat");
            netio_pkt_finv(pi->l2Data, pi->allHeadersSize);
            netio_pkt_fence();
            netio_error_t err = NETIO_QUEUE_FULL;
            while (err == NETIO_QUEUE_FULL) {
                err = netio_send_packet(&t->queue, pi->pkt);
            }
            if (err == NETIO_NO_ERROR) {
                ++t->send[rt.index];
                return 0;
            }
            error("%02d:nnetio_send_packet(%p, %p) returned %d: %s",
                  t->index, &t->queue, pi->pkt,
                  err, netio_strerror(err));
        }
        error("%02d: No route for port %d", t->index, pi->poa);
    } else {
        error("%02d: Drop packet with bad status %d: %s",
              t->index, pi->status, netio_strerror(pi->status));
    }
    ++t->drop[rt.index];
    return 1;
}


// Dispatch the NETIO packet at pkt from t->queue.  Return 1 if the packet
// buffer must be freed with netio_free_buffer(&t->queue, pkt).  Otherwise
// return 0.
//
static int forwardPacketOnQueueOrTap(Thread *t, netio_pkt_t *pkt)
{
    INFO("%02d: forwardPacketOnQueueOrTap(%p, %p)", t->index, t, pkt);
    Process *const p = t->process;
    int result = 1;
    const PacketInfo pi = parsePacket(p, pkt);
    ++t->status[pi.status];
    if (pi.isUdpForMe) return forwardPacketOnQueueOrDrop(t, &pi);
    ++t->tap;
    const int wCount = write(p->tap, pi.l2Data, pi.l2Length);
    if (wCount < 0) {
        error("%02d: write(%d, %p, %zu) returned %d with errno %d: %s",
              t->index, p->tap, pi.l2Data, pi.l2Length, wCount,
              errno, strerror(errno));
    }
    return 1;
}


// Forward a UDP packet for t->queue.
//
static void forwardPackets(Thread *t)
{
    // INFO("%02d: forwardPackets(%p)", t->index, t); // too much spew
    netio_queue_t *const q = &t->queue;
    int packetSent = 0;
    netio_pkt_t pkt;
    netio_error_t err = netio_get_packet(q, &pkt);
    if (err != NETIO_NOPKT) {
        if (err == NETIO_NO_ERROR) {
            const int freePacketBuffer = forwardPacketOnQueueOrTap(t, &pkt);
            if (freePacketBuffer) {
                err = netio_free_buffer(q, &pkt);
                if (err != NETIO_NO_ERROR) {
                    error("%02d: netio_free_buffer(%p, %p) returned %d: %s",
                          t->index, q, &pkt, err, netio_strerror(err));
                }
            }
        } else {
            error("%02d: netio_get_packet(%p, %p) returned %d: %s",
                  t->index, q, &pkt, err, netio_strerror(err));
        }
    }
}


void *forwardStart(void *v)
{
    Thread *const t = (Thread *)v;
    Process *const p = t->process;
    INFO("%02d: forwardStart(p)", t->index, t);
    const int fail = tmc_cpus_set_my_cpu(t->cpu);
    if (fail) {
        error("%02d: tmc_cpus_set_my_cpu(%d) returned %d for thread %2d",
              t->index, t->cpu, fail, t->index);
    }
    registerQueueReadWrite(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    while (!t->alert) forwardPackets(t);
    INFO("%02d: forwardStart(%p) alerted", t->index, t);
    unregisterQueue(t);
    processLock(p); t->alert = 0; processNotify(p); processUnlock(p);
    return t;
}
