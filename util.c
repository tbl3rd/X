#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "util.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


static const char *the_whiner = "switch";


// Establish whiner as source of error messages if not 0.
// Return the prior whiner.
//
const char *errorInitialize(const char *whiner)
{
    const char *result = the_whiner;
    if (whiner) the_whiner = whiner;
    return result;
}


// fprintf(stderr, "%s: ERROR: " format, errorInitialize(0), ...);
//
void error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s: ERROR: ", errorInitialize(0));
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}


// fprintf(stderr, "%s: INFO: " format, errorInitialize(0), ...);
//
void info(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s: INFO: ", errorInitialize(0));
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}


// fprintf(stderr, "%s: SHOW: " format, errorInitialize(0), ...);
//
void show(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s: SHOW: ", errorInitialize(0));
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}


// Write into noa the 4 bytes of the IPv4 address whose string is ips.
// Return 0 if something goes wrong.  Otherwise return something else.
//
int ipFromString(unsigned char noa[4], const char *ips)
{
    unsigned int i[4] = {};
    const int count = sscanf(ips, IPFMT, &i[0], &i[1], &i[2], &i[3]);
    const int result = (count == 4) &&
        (i[0] >= 0 && i[0] < 256) &&
        (i[1] >= 0 && i[1] < 256) &&
        (i[2] >= 0 && i[2] < 256) &&
        (i[3] >= 0 && i[3] < 256);
    if (result) {
        noa[0] = (unsigned char)(i[0] & 0xff);
        noa[1] = (unsigned char)(i[1] & 0xff);
        noa[2] = (unsigned char)(i[2] & 0xff);
        noa[3] = (unsigned char)(i[3] & 0xff);
    }
    return result;
}


// Write into noa the 6 bytes of the MAC address whose string is mac.
// Return 0 if something goes wrong.  Otherwise return something else.
//
int macFromString(unsigned char noa[6], const char *mac)
{
    int i[6] = {};
    const int count =
        sscanf(mac, MACSCANFMT, &i[0], &i[1], &i[2], &i[3], &i[4], &i[5]);
    const int result = (count == 6) &&
        (i[0] >= 0 && i[0] < 256) &&
        (i[1] >= 0 && i[1] < 256) &&
        (i[2] >= 0 && i[2] < 256) &&
        (i[3] >= 0 && i[3] < 256) &&
        (i[4] >= 0 && i[4] < 256) &&
        (i[5] >= 0 && i[5] < 256);
    if (result) {
        noa[0] = (unsigned char)(i[0] & 0xff);
        noa[1] = (unsigned char)(i[1] & 0xff);
        noa[2] = (unsigned char)(i[2] & 0xff);
        noa[3] = (unsigned char)(i[3] & 0xff);
        noa[4] = (unsigned char)(i[4] & 0xff);
        noa[5] = (unsigned char)(i[5] & 0xff);
    }
    return result;
}


// Return 0 if ips is not a valid IP address string.  Return something else
// if ips is a valid IPv4 4-component, dotted-decimal address string --
// that is A.B.C.D where A, B, C, and D are decimal integers in [0,256).
//
int validIpString(const char *ips)
{
    unsigned char ignored[4];
    return ipFromString(ignored, ips);
}


// Return 0 if mac is not a valid MAC address string.  Return something
// else if mac is a valid 6-component, hex-and-colon MAC address string
// -- that is A:B:C:D:E:F where A, B, C, D, E, and F are hexadecimal
// integers in [0x00,0xff].
//
int validMacString(const char *mac)
{
    unsigned char ignored[6];
    return macFromString(ignored, mac);
}


int bindUdpPort(const char *ips, int port)
{
    INFO("__: bindUdpPort(%s, %d)", ips, port);
    int result = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (result == -1) {
        error("__: socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) "
              "returned %d with errno: %s", result, errno, strerror(errno));
    } else {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
        };
        const int ok = inet_aton(ips, &addr.sin_addr);
        if (ok) {
            const int fail =
                bind(result, (struct sockaddr *)&addr, sizeof addr);
            if (fail) {
                error("__: bind(%d, %p, %z) returned %d with errno %d: %s",
                      result, &addr, sizeof addr, fail, errno, strerror(errno));
                close(result);
                result = -1;
            } else {
                INFO("__: bindUdpPort(%s, %d) opened %d on %s",
                     ips, port, result, inet_ntoa(addr.sin_addr));
            }
        } else {
            error("__: inet_aton(%s, %p) returned %d",
                  ips, &addr.sin_addr, ok);
        }
    }
    return result;

}


int connectUdpPort(const char *ips, int port)
{
    INFO("__: connectUdpPort(%s, %d)", ips, port);
    int result = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (result == -1) {
        error("__: socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) "
              "returned %d with errno: %s", result, errno, strerror(errno));
    } else {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port)
        };
        const int ok = inet_aton(ips, &addr.sin_addr);
        if (ok) {
            const int fail =
                connect(result, (struct sockaddr *)&addr, sizeof addr);
            if (fail) {
                error("__: connect(%d, %p, %z) returned %d with errno %d: %s",
                      result, &addr, sizeof addr, fail, errno, strerror(errno));
                close(result);
                result = -1;
            } else {
                INFO("__: connectUdpPort(%s, %d) opened %d on %s",
                     ips, port, result, inet_ntoa(addr.sin_addr));
            }
        } else {
            error("__: inet_aton(%s, %p) returned %d",
                  ips, &addr.sin_addr, ok);
        }
    }
    return result;
}


int connectTcpPort(const char *ips, int port)
{
    info("__: connectTcpPort(%s, %d)", ips, port);
    int result = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (result == -1) {
        error("__: socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) "
              "returned %d with errno: %s", result, errno, strerror(errno));
    } else {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
        };
        const int ok = inet_aton(ips, &addr.sin_addr);
        if (ok) {
            const int fail =
                connect(result, (struct sockaddr *)&addr, sizeof addr);
            if (fail) {
                error("__: connect(%d, %p, %zu) returned %d with errno %d: %s",
                      result, &addr, sizeof addr, fail, errno, strerror(errno));
                close(result);
                result = -1;
            } else {
                INFO("__: connectTcpPort(%s, %d) opened %d on %s",
                     ips, port, result, inet_ntoa(addr.sin_addr));
            }
        } else {
            error("__: inet_aton(%s, %p) returned %d",
                  ips, &addr.sin_addr, ok);
        }
    }
    return result;
}


int listenTcpPort(const char *ips, int port)
{
    INFO("__: listenTcpPort(%s, %d)", ips, port);
    int result = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (result == -1) {
        error("__: socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) "
              "returned %d with errno: %s", result, errno, strerror(errno));
    } else {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
        };
        const int ok = inet_aton(ips, &addr.sin_addr);
        if (ok) {
            const int fail =
                bind(result, (struct sockaddr *)&addr, sizeof addr);
            if (fail) {
                error("__: bind(%d, %p, %z) returned %d with errno %d: %s",
                      result, &addr, sizeof addr, fail, errno, strerror(errno));
                close(result);
                result = -1;
            } else {
                INFO("__: listenTcpPort(%s, %d) opened %d on %s",
                     ips, port, result, inet_ntoa(addr.sin_addr));
                const int fail = listen(result, 0);
                if (fail) {
                    error("__: listen(%d, 0) returned %d with errno %d: %s",
                          result, fail, errno, strerror(errno));
                    close(result);
                    result = -1;
                }
            }
        } else {
            error("__: inet_aton(%s, %p) returned %d",
                  ips, &addr.sin_addr, ok);
        }
    }
    return result;
}


// Run system(cmd, ...) and report its exit status unless it is 0.
//
void systemCommand(const char *cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    char buffer[256];
    vsnprintf(buffer, sizeof buffer, cmd, args);
    va_end(args);
    buffer[sizeof buffer - 1] = ""[0];
    info("__: system(%s)", buffer);
    const int status = system(buffer);
    if (status != 0) {
        error("__: system(%s) returned %d with errno %d: %s",
              buffer, status, errno, strerror(errno));
    }
}


// Just use the first local non-loopback IPv4 address.
//
void getControlIp(unsigned char noa[4])
{
    struct ifaddrs *ifap = NULL;
    if (0 == getifaddrs(&ifap)) {
        struct ifaddrs *p = ifap;
        for (p = ifap; p; p = p->ifa_next) {
            const unsigned int flags = p->ifa_flags;
            const int family = p->ifa_addr->sa_family;
            const int match = p->ifa_addr && (family == AF_INET) &&
                !(flags & IFF_LOOPBACK);
            if (match) {
                char ips[] = "255.255.255.255";
                struct sockaddr_in *saddr_in = NULL;
                const int fail =
                    getnameinfo(p->ifa_addr, sizeof *saddr_in,
                                ips, sizeof ips, 0, 0, NI_NUMERICHOST);
                if (fail) {
                    error("__: getnameinfo(%p, %zu, %p, %zu, 0, 0, "
                          "NI_NUMERICHOST) returned %d:%s",
                          p->ifa_addr, sizeof *saddr_in, ips, sizeof ips,
                          fail, gai_strerror(fail));
                } else {
                    INFO("__: getControlIp() ips == '%s'", ips);
                    ipFromString(noa, ips);
                }
            }
        }
        freeifaddrs(ifap);
    }
}


// Send on fd a size 0 route control string to the switch to shut it down.
//
void stopSwitch(int fd)
{
    INFO("__: stopSwitch(%d)", fd);
    const int zero = 0;
    const int zeroSize = write(fd, &zero, sizeof zero);
    if (zeroSize != sizeof zero) {
        error("__: write(%d, %p, %zu) returned %d with errno %d: %s",
              fd, &zero, sizeof zero, zeroSize, errno, strerror(errno));
    }
    const int eofSize = write(fd, "", 0);
    if (eofSize != 0) {
        error("__: write(%d, \"\", 0) returned %d with errno %d: %s",
              fd, eofSize, errno, strerror(errno));
    }
    close(fd);
}
