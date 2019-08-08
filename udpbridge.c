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

#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>

#include <netdb.h>

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

// Sockets and tap are different, so wrap behind interface.
struct port;
typedef ssize_t (*port_read)(struct port *, uint8_t *, ssize_t);
typedef ssize_t (*port_write)(struct port *, uint8_t *, ssize_t);
struct port {
    int fd;
    port_read read;
    port_write write;
};

static ssize_t fd_read(struct port *p, uint8_t *buf, ssize_t len) {
    ssize_t rlen;
    ASSERT_ERRNO(rlen = read(p->fd, buf, len));
    return rlen;
}
static ssize_t fd_write(struct port *p, uint8_t *buf, ssize_t len) {
    ssize_t wlen;
    ASSERT_ERRNO(wlen = write(p->fd, buf, len));
    return wlen;
}

static inline struct port *open_tap(char *dev) {
    int fd;
    ASSERT_ERRNO(fd = open("/dev/net/tun", O_RDWR));
    struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI };
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    ASSERT_ERRNO(ioctl(fd, TUNSETIFF, (void *) &ifr));
    LOG("tap: %s\n", dev);
    struct port *port;
    ASSERT(port = malloc(sizeof(*port)));
    port->fd = fd;
    port->read = fd_read;
    port->write = fd_write;
    return port;
}

struct udp_port {
    struct port p;
    struct sockaddr_in peer;
};

static ssize_t udp_read(struct udp_port *p, uint8_t *buf, ssize_t len) {
    /* NOTE THAT THIS IS VERY INSECURE
       We send data where ever we got it from last.
       FIXME: At least do an IP check or something. */
    ssize_t rlen;
    int flags = 0;
    socklen_t addrlen = sizeof(&p->peer);
    ASSERT_ERRNO(
        rlen = recvfrom(p->p.fd, buf, len, flags,
                        (struct sockaddr*)&p->peer, &addrlen));
    //LOG("addrlen %d %d\n", addrlen, sizeof(p->peer));
    ASSERT(addrlen == sizeof(p->peer));
    return rlen;
}
static ssize_t udp_write(struct udp_port *p, uint8_t *buf, ssize_t len) {
    if (p->peer.sin_port == 0) {
        LOG("drop %d\n", (int)len);
        return len; // this is not an error
    }
    ssize_t wlen;
    int flags = 0;
    ASSERT_ERRNO(
        wlen = sendto(p->p.fd, buf, len, flags,
                      (struct sockaddr*)&p->peer,
                      sizeof(p->peer)));
    return wlen;
}

static inline struct port *open_udp(u_short port) {
    int fd;
    struct sockaddr_in address = {
        .sin_port = htons(port),
        .sin_family = AF_INET
    };
    ASSERT_ERRNO(fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    ASSERT_ERRNO(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)));
    socklen_t addrlen = sizeof(address);
    ASSERT_ERRNO(bind(fd, (struct sockaddr *)&address, addrlen));
    LOG("udp: %d\n", port);

    struct udp_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.fd = fd;
    p->p.read  = (port_read)udp_read;
    p->p.write = (port_write)udp_write;

    //struct hostent *hp;
    //ASSERT(hp = gethostbyname(host));
    //memcpy((char *)&p->peer.sin_addr, (char *)hp->h_addr, hp->h_length);
    //p->peer.sin_port = htons(port);
    //p->peer.sin_family = AF_INET;
    return &p->p;
}

/* FIXME: I'm fundamentally misunderstanding something.  I want all
 * incoming UDP packets to arrive at the tap, and I want to send the
 * tap incoming to a particular host. */


static inline void proxy(struct port **port) {
    struct pollfd pfd[2] = {};
    for (int i=0; i<2; i++) {
        pfd[i].fd = port[i]->fd;
        pfd[i].events = POLLERR | POLLIN;
    }
    for(;;) {
        uint8_t buf[4096];
        int rv;
        ASSERT_ERRNO(rv = poll(&pfd[0], 2, -1));
        ASSERT(rv >= 0);
        for (int i=0; i<2; i++) {
            if(pfd[i].revents & POLLIN) {
                int rlen = port[i]->read(port[i], buf, sizeof(buf));
                LOG("%d %d\n", i, rlen);
                int wlen = port[1-i]->write(port[1-i], buf, rlen);
                ASSERT(rlen == wlen);
            }
        }
    }
}

static inline int main_udp2tap(int argc, char **argv) {
    ASSERT(argc > 2);
    struct port* port[] = {
        open_tap(argv[1]),
        open_udp(atoi(argv[2]))
    };
    //test_send(fd[0]);
    //test_recv(fd[0], argv[1]);
    proxy(&port[0]);

    return 0;
}
static inline int main_udp2udp(int argc, char **argv) {
    return 0;
}