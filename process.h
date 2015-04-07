#ifndef INCLUDE_PROCESS_H
#define INCLUDE_PROCESS_H


// Manage threads in a switch or tester process.


#include <pthread.h>
#include <netio/netio.h>

#include "route.h"
#include "util.h"


// The number of CPU tiles on our Tilera chips.
//
#define MAXCPUCOUNT (64)


// State for this thread in the process.
//
// .index is this thread's index into the Process.thread array.
// .cpu is the Tilera CPU ID of the tile this thread runs on.
// .alert is set by main() and cleared by this thread to synchronize.
// .queue is the thread's NETIO queue.
// .start is the function this thread started with or 0 for main().
// .self is this thread's pthread ID.
// .process is a pointer back to the Process state shared with others.
// .drop[n] is a count of dropped packets from port (PORTOFFSET + n).
// .recv[n] is a count of packets received from port (PORTOFFSET + n).
// .send[n] is a count of packets sent from port (PORTOFFSET + n).
// .status is a count of packets indexed by netio_pkt_status_t.
// .tap is a count of packets forwarded to the TAP interface.
//
typedef struct Thread {
    int index;
    int cpu;
    int alert;                          // shared via .process
    netio_queue_t queue;
    void *(*start)(void *);
    pthread_t self;
    struct Process *process;
    unsigned long long drop[R30TOTALCHANNELS];
    unsigned long long recv[R30TOTALCHANNELS];
    unsigned long long send[R30TOTALCHANNELS];
    unsigned long long status[NETIO_PKT_STATUS_BAD + 1];
    unsigned long long tap;
} Thread;


// State shared by threads in the process.
//
// .av0 is the name of the program running in this process.
// .interface is the name of the network interface this process uses.
//
// .forward describes this process's network endpoint for UDP forwarding.
// .forward.port is not used.
// .forward.ip is this process's IPv4 forwarding address.
// .forward.mac is the MAC address on .interface with the .forward.ip address.
//
// .control describes the switch's control endpoint to both switch and tester.
// .control.port is the port on which this listens for control messages.
// .control.ip is the switch's control IP address for the tester program.
// .control.mac is not used.
//
// .tap is the file descriptor of the interface's TAP device.
// .packetCount is the number of packets to send from the tester.
// .routeCount is the number of route commands handled.
// .threadCount is the number of active threads in .thread.
// .thread is an array of per-thread state for .threadCount threads.
// .netioThreadCount is the number of NETIO threads.
// .netioThreadOffset is the index of the first NETIO thread.
// .attr points to the attribute structure used to create threads.
// .using is a mutex to hold when using the shared process state.
// .changed is a condition for changing shared process state.
//
// Together .using and .changed comprise a monitor over the process state
// shared among threads.
//
typedef struct Process {
    const char *av0;
    const char *interface;
    Endpoint forward;
    Endpoint control;
    int tap;
    int packetCount;
    int routeCount;
    int threadCount;
    Thread thread[MAXCPUCOUNT];
    int netioThreadCount;
    int netioThreadIndex;
    pthread_attr_t *attr;
    pthread_mutex_t using;
    pthread_cond_t changed;
} Process;


// Manage the shared process state monitor.
//
extern void processLock(Process *p);
extern void processUnlock(Process *p);
extern void processNotify(Process *p);
extern void processWait(Process *p);

// Stop the threads in p running start(), and return a count of the threads
// after they stop.
//
extern int processStopThreads(Process *p, void *(*start)(void *),
                              const char *name);

// Start all threads in p that are set up to run start().  Set pthread
// stack to the smallest permitted size.  Return the number of threads
// started after they've all started running.
//
extern int processStartThreads(Process *p, void *(*start)(void *),
                               const char *name);

// Return a pointer to this process initialized with name av0 and threads
// ready to start.  Bind the caller to the 0th CPU.  Set up other threads
// to run tapStart() on the "first CPU", and (*start)() on the rest.
//
extern Process *processInitialize(const char *av0, void *(*start)(void *),
                                  const char *name);

// Cleanup any remaining process state after all non-main() threads are
// stopped.
//
extern void processUninitialize(Process *p);


#endif // INCLUDE_PROCESS_H
