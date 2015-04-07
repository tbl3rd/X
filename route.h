#ifndef INCLUDE_ROUTE_H
#define INCLUDE_ROUTE_H


// Manage routes in a switch or tester process.


// A network endpoint.
//
// .port is the UDP port number.
// .ip is the 4-byte IPv4 address.
// .mac is the 6-byte MAC (Ethernet) address.
//
typedef struct Endpoint {
    int port;
    unsigned char ip[4];
    unsigned char mac[6];
} Endpoint;


// A route forwarded by the switch that maps an input port .poa to an
// output port .dst.port with the corresponding ip and mac addresses.
//
// .index is an index into the route[] table (route[n].index == n).
// .poa is the UDP port of arrival (route[n].poa == n + PORTOFFSET).
// .dst is the destination endpoint for the packets.
// .open is true if the route is active and false if closed.
//
typedef struct Route {
    int index;
    int poa;
    Endpoint dst;
    int open;
} Route;

// Initialize the routing table.
//
extern void routeInitialize(void);

// Open route r to start forwarding packets arriving on r->poa, and return
// a pointer to a stable record of the route.
//
extern void routeOpen(const Route *r);

// Close out route r to start discarding packets arriving on r->poa.
//
extern void routeClose(const Route *r);

// Return a pointer to the route for poa.
//
extern const Route routeFromPortOfArrival(int poa);

// Return a route described by the JSON string s.
//
extern const Route routeFromString(const char *s);

// Write into buffer up to size bytes of a JSON string describing route.
// Return -1 or a count of the bytes written at buffer.
//
extern int routeToString(const Route *r, char *buffer, size_t size);

// Send route at r on fd.
//
extern void routeSendControl(int fd, const Route *r);


#endif // INCLUDE_ROUTE_H
