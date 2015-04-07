#ifndef INCLUDE_PACKETS_H
#define INCLUDE_PACKETS_H


// Send and receive packets for the tester program.

struct Thread;                          // defined in process.h

// Prime the packets pipeline by sending a zeroth packet on all open routes.
//
void packetsPrimePipeline(struct Thread *t);

// Start receiving and sending packets.  This is a pthread_create() start
// function where thread is a (Thread *) cast to (void *).
//
extern void *packetsStart(void *thread);


#endif // INCLUDE_PACKETS_H
