#ifndef INCLUDE_TAP_H
#define INCLUDE_TAP_H


struct Process;                         // Defined in process.h.


// Manage a TAP device for forwarding unrouted UDP packets received on
// any NETIO queue.

// Configure the TAP device for p writing its file descriptor in p->tap.
//
extern void tapConfigure(struct Process *p);

// Manage a TAP device on thread.  This is a pthread_create() start
// function where thread is a (Thread *) cast to (void *).
//
extern void *tapStart(void *v);

// Close fd to shut down the TAP forwarder.
//
void tapStop(int fd);


#endif // INCLUDE_TAP_H
