#ifndef INCLUDE_UTIL_H
#define INCLUDE_UTIL_H


// Declare some utility functions shared between programs that do not
// depend on Tilera.


// Define SLEEP(X) as sleep(X) to slow this bird down for debugging.
//
// #define SLEEP(X) sleep(X)
#define SLEEP(X)

// The standard range of ephemeral socket ports is 49152 to 65535
// (or 0x0c000 to 0x10000).  The router needs 3600 to support an
// R30: #ofS2Qs * #TileraSockets * #ChannelsPerSocket or 30*8*15.
// Round it up to 4000 by increasing #ChannelsPerSocket to 16.
//
#define CHANNELSPERSOCKET (16)
#define SOCKETSPERS2Q (8)
#define R30S2QCOUNT (30)
#define MINEPHEMERALSOCKETPORT (0x0c000) // 49152
#define MAXEPHEMERALSOCKETPORT (0x10000) // 65535
#define R30TOTALCHANNELS (R30S2QCOUNT * SOCKETSPERS2Q * CHANNELSPERSOCKET)
#define PORTOFFSET (50000)              // between 49152 and 65535
#define CONTROLPORT (PORTOFFSET + R30TOTALCHANNELS)

// The name of an interface for forwarding UDP packets in production.
// This should be the fastest available interface.
//
#define PRODUCTIONINTERFACE "xgbe/0"

// The name of an interface for forwarding UDP packets for convenience.
// This should not require special connections such as optical cable.
//
#define CONVENIENCEINTERFACE "gbe/0"

// An IP address for the UDP switch's forwarding interface to use in
// examples and so on.
//
#define EXAMPLEFORWARDINGIP "172.18.11.200"


#define IPFMT "%d.%d.%d.%d"
#define MACFMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MACSCANFMT "%x:%x:%x:%x:%x:%x"


// Parse or print a JSON route command string.
//
#define JSONROUTEFMT \
    "    { \"from\" : %d ,                 \n" \
    "      \"port\" : %d ,                 \n" \
    "      \"ip\"   : \"" IPFMT "\" ,      \n" \
    "      \"mac\"  : \"" MACSCANFMT "\" } \n"


// Establish whiner as source of error info and show messages if not 0.
// Return the current whiner.
//
extern const char *errorInitialize(const char *whiner);

// fprintf(stderr, "%s: ERROR: " format "\n", errorInitialize(0), ...);
//
extern void error(const char *format, ...);

// fprintf(stderr, "%s: INFO: " format, errorInitialize(0), ...);
// This is usually defined away as INFO(...).
//
extern void info(const char *format, ...);

// fprintf(stderr, "%s: SHOW: " format, errorInitialize(0), ...);
//
extern void show(const char *format, ...);

// Write into noa the 4 bytes of the IPv4 address whose string is ips.
// Return 0 if something goes wrong.  Otherwise return something else.
//
extern int ipFromString(unsigned char noa[4], const char *ips);

// Write into noa the 6 bytes of the MAC address whose string is mac.
// Return 0 if something goes wrong.  Otherwise return something else.
//
extern int macFromString(unsigned char noa[6], const char *mac);

// Return 0 if ips is not a valid IP address string.  Return something else
// if ips is a valid IPv4 4-component, dotted-decimal address string --
// that is A.B.C.D where A, B, C, and D are decimal integers in [0,256).
//
extern int validIpString(const char *ips);

// Return 0 if mac is not a valid MAC address string.  Return something
// else if mac is a valid 6-component, hex-and-colon MAC address string
// -- that is A:B:C:D:E:F where A, B, C, D, E, and F are hexadecimal
// integers in [0x00,0xff).
//
extern int validMacString(const char *mac);

// Return -1 or a UDP socket bound to IPv4 address ip and port (ip:port).
// A read() from the resulting file descriptor returns a UDP packet sent to
// the forward ip:port.
//
extern int bindUdpPort(const char *ips, int port);

// Return -1 or a UDP socket connected to IPv4 address ip and port
// (ip:port).  A write() to the resulting file descriptor delivers a UDP
// packet to the forwarding ip:port.
//
extern int connectUdpPort(const char *ips, int port);

// Return -1 or a TCP socket connected to IPv4 address ip and port
// (ip:port).  A write() to the resulting file descriptor can deliver
// a route control command string to the control ip:port.
//
extern int connectTcpPort(const char *ips, int port);

// Return -1 or a TCP socket bound and listening to IPv4 address ip and
// port (ip:port).  A read() from the resulting file descriptor can return
// a route control command sent to control ip:port.
//
extern int listenTcpPort(const char *ips, int port);

// Run system(cmd, ...) and report its exit status unless it is 0.
//
extern void systemCommand(const char *cmd, ...);

// Write into noa 4 bytes of the caller's control IPv4 address.  The
// control address is the one on the network interface managed by Linux.
//
extern void getControlIp(unsigned char noa[4]);

// Send on fd a size 0 route control string to the switch to shut it down.
//
extern void stopSwitch(int fd);


#endif // INCLUDE_UTIL_H
