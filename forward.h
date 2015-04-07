#ifndef INCLUDE_FORWARD_H
#define INCLUDE_FORWARD_H


// Receive and send packets to forward them according to route commands
// in the switch program.

// Start forwarding UDP packets according to their port of arrival on
// thread.  This is a pthread_create() start function where thread is
// a (Thread *) cast to (void *).
//
extern void *forwardStart(void *thread);


#endif // INCLUDE_FORWARD_H
