#include "system.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

// https://stackoverflow.com/questions/1003684/how-to-interface-with-the-linux-tun-driver
// https://www.kernel.org/doc/Documentation/networking/tuntap.txt
// https://wiki.wireshark.org/Development/LibpcapFileFormat

static inline void log_packet(uint8_t *buf, ssize_t n) {
    for(ssize_t i=0; i<n; i++) {
        LOG(" %02x", buf[i]);
        if (i%16 == 15) LOG("\n");
    }
    LOG("\n");
}

static inline void test_send(int fd) {
    while(1) {
        sleep(1);
        uint8_t test[] = {
            0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,
            0x20,0x20,
            1,2,3,4
        };
        write(fd, &test[0], sizeof(test));
    }
}
static inline void test_recv(int fd, const char *ifname) {
    while(1) {
        uint8_t buf[2048];
        ssize_t nbytes;
        ASSERT((nbytes = read(fd, &buf[0], sizeof(buf))) > 0);
        LOG("%s: (%d)\n", ifname, (int)nbytes);
        log_packet(&buf[0], nbytes);
    }
}

static inline int open_tap(char *dev) {
    int fd;
    ASSERT_ERRNO(fd = open("/dev/net/tun", O_RDWR));
    LOG("%s %d\n", dev, fd);
    struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI };
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    ASSERT_ERRNO(ioctl(fd, TUNSETIFF, (void *) &ifr));
    return fd;
}

static inline int main_udp2tap(int argc, char **argv) {
    ASSERT(argc > 1);
    int fd = open_tap(argv[1]);
    //test_send(fd);
    test_recv(fd, argv[1]);
    return 0;
}
static inline int main_udp2udp(int argc, char **argv) {
    return 0;
}
