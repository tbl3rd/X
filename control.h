#ifndef INCLUDE_CONTROL_H
#define INCLUDE_CONTROL_H


// Listen on CONTROLPORT for JSON route controls sent to the switch.

// Defined in process.h.
//
struct Thread;

// Use t to listen for JSON route control strings on the
// t->process->control file descriptor.
//
// On EOF return the number of routing commands received.
//
extern int controlRoutes(struct Thread *t);


#endif // INCLUDE_CONTROL_H
