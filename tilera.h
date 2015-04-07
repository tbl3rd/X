#ifndef INCLUDE_TILERA_H
#define INCLUDE_TILERA_H


// Declare some shared functions that depend on Tilera.


#include "process.h"


// Register a NETIO queue for thread t.  Register t->queue for reading,
// reading and writing, or just writing.  registerQueueStatsOnly() sets
// t->queue up for monitoring interface statistics without enabling IO.
//
extern void registerQueueRead(Thread *t);
extern void registerQueueReadWrite(Thread *t);
extern void registerQueueWrite(Thread *t);
extern void registerQueueStatsOnly(Thread *t);

// Unregister the queue for t (t->queue) with NETIO.
//
extern void unregisterQueue(Thread *t);

// Initialize Tilera's NETIO for the routing process p.  Set up to register
// one queue per tile, with tile 0 listening for commands, tile 1 writing
// the TAP device, and tiles 2 and above routing UDP packets.
//
extern void initializeNetio(Process *p);


// Return a PacketInfo describing the NETIO packet at pkt for p.
//
// .isUdpForMe is true if .pkt is a UDP packet for the caller's interface.
// .poa is 0 or the port of arrival for the UDP packet if .isUdpForMe.
// .pkt is the NETIO packet buffer.
// .md is the NETIO packet metadata.
// .status is the NETIO packet status.
// .l2Data points to the Ethernet header.
// .l3Data points to the IPv4 header.
// .l2Length is the size in bytes of the Ethernet packet.
// .l3Length is the size in bytes of the IPv4 packet.
// .ipHeaderSize is the calculated size of the IPv4 header in bytes.
// .allHeadersSize is the calculated size of all the frame headers,
//                 such that .l2Data + .allHeadersSize points to the
//                 packet's payload data.
//
typedef struct PacketInfo {
    int isUdpForMe;
    int poa;
    netio_pkt_t *pkt;
    netio_pkt_metadata_t *md;
    netio_pkt_status_t status;
    unsigned char *l2Data;
    unsigned char *l3Data;
    unsigned int l2Length;
    unsigned int l3Length;
    unsigned int ipHeaderSize;
    unsigned int allHeadersSize;
} PacketInfo;
extern const PacketInfo parsePacket(const Process *p, netio_pkt_t *pkt);

// Show all the counters in p on the INFO log.
//
extern void showCounters(Process *p);

// Dump the packet at pkt into file.
//
extern void dumpPacket(netio_pkt_t *pkt, const char *file);


#endif // INCLUDE_TILERA_H
